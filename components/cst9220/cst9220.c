#include "cst9220.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#define CST9220_REG_TOUCH_HI 0xD0
#define CST9220_REG_TOUCH_LO 0x00
#define CST9220_ACK          0xAB
#define CST9220_MAX_POINTS   2
#define CST9220_REPORT_LEN   15
#define CST9220_TIMEOUT_MS   20
#define CST9220_PROJECT_LEGACY 0x542F

static const char *TAG = "cst9220";

typedef struct {
    esp_lcd_touch_t base;
    i2c_master_dev_handle_t i2c_dev;
    uint32_t report_sequence;
    uint16_t project_id;
    bool legacy_protocol;
    uint8_t track_id[CST9220_MAX_POINTS];
} cst9220_t;

static esp_err_t cst9220_write_command(cst9220_t *ctx, uint16_t command)
{
    const uint8_t data[] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_master_transmit(ctx->i2c_dev, data, sizeof(data), CST9220_TIMEOUT_MS);
}

static esp_err_t cst9220_read_command(cst9220_t *ctx, uint16_t command, uint8_t *data, size_t len)
{
    const uint8_t address[] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_master_transmit_receive(ctx->i2c_dev, address, sizeof(address), data, len, CST9220_TIMEOUT_MS);
}

static esp_err_t cst9220_enter_report_mode(cst9220_t *ctx)
{
    /* Espressif esp_lcd_touch_cst9220 v0.1.1: confirmed project 0x542F
     * reports on D109 without ACK; current/unknown projects use D101+ACK.
     * Select by identity, never by ignoring a failed report acknowledgement. */
    ESP_RETURN_ON_ERROR(cst9220_write_command(ctx, ctx->legacy_protocol ? 0xD109 : 0xD101),
                        TAG, "enter report mode");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t cst9220_reset(const esp_lcd_touch_config_t *config)
{
    if (config->rst_gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << config->rst_gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "configure reset");
    gpio_set_level(config->rst_gpio_num, config->levels.reset);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(config->rst_gpio_num, !config->levels.reset);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static void cst9220_clear_data(esp_lcd_touch_handle_t tp)
{
    cst9220_t *ctx = (cst9220_t *)tp;
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    memset(ctx->track_id, 0, sizeof(ctx->track_id));
    portEXIT_CRITICAL(&tp->data.lock);
}

static esp_err_t cst9220_read_data(esp_lcd_touch_handle_t tp)
{
    cst9220_t *ctx = (cst9220_t *)tp;
    uint8_t report[CST9220_REPORT_LEN] = {0};
    esp_err_t err = cst9220_read_command(ctx, 0xD000, report, sizeof(report));
    if (err != ESP_OK) {
        /* The adapter cancels its logical gesture on transport errors. Keep
         * the last physical state so an empty recovery poll cannot invent
         * the real release that ends its consume-until-release guard. */
        return err;
    }

    if (!ctx->legacy_protocol) {
        const uint8_t ack[] = {CST9220_REG_TOUCH_HI, CST9220_REG_TOUCH_LO, CST9220_ACK};
        err = i2c_master_transmit(ctx->i2c_dev, ack, sizeof(ack), CST9220_TIMEOUT_MS);
    }

    /* ACK echoes and markerless polls contain no fresh point snapshot. Keep
     * physical state until a marked report updates it; inventing a release
     * here turns a drag/hold into repeated clicks at the LVGL polling rate. */
    if (report[0] == CST9220_ACK || report[6] != CST9220_ACK) {
        ESP_RETURN_ON_ERROR(err, TAG, "acknowledge report");
        return ESP_OK;
    }

    esp_lcd_touch_point_data_t decoded[CST9220_MAX_POINTS] = {0};
    uint8_t decoded_count = 0;
    bool released[16] = {0};
    bool invalid_active = false;
    const uint8_t reported_points = report[5] & 0x7F;
    {
        /* A zero first byte can be ID0's explicit release record while another
         * finger remains down. Only the marked record statuses/count decide. */
        uint8_t points = reported_points;
        if (points > CST9220_MAX_POINTS) {
            points = CST9220_MAX_POINTS;
        }
        for (uint8_t i = 0; i < points; ++i) {
            const uint8_t *p = report + i * 5 + (i == 0 ? 0 : 2);
            const uint8_t id = p[0] >> 4;
            const uint8_t event = p[0] & 0x0F;
            /* The low status bit is not the press/release discriminator;
             * active reports have status 6 or 7. IDs are the controller's
             * four-bit tracking IDs, not indices into our two-point array. */
            if ((event >> 1) != 3) {
                released[id] = true;
                continue;
            }
            const uint16_t x = (uint16_t)((p[1] << 4) | (p[3] >> 4));
            const uint16_t y = (uint16_t)((p[2] << 4) | (p[3] & 0x0F));
            if (x >= tp->config.x_max || y >= tp->config.y_max) {
                invalid_active = true;
                continue;
            }
            decoded[decoded_count++] = (esp_lcd_touch_point_data_t){.x = x, .y = y, .track_id = id};
        }
    }
    portENTER_CRITICAL(&tp->data.lock);
    if (!invalid_active) {
        tp->data.points = decoded_count;
        for (uint8_t i = 0; i < decoded_count; ++i) {
            tp->data.coords[i] = decoded[i]; ctx->track_id[i] = decoded[i].track_id;
        }
        if (reported_points <= CST9220_MAX_POINTS) {
            /* Leave zero reserved for handles that have no report. */
            if (++ctx->report_sequence == 0) ++ctx->report_sequence;
        }
    } else {
        /* Reject a corrupt active coordinate instead of synthesizing release.
         * Explicit release records still retire their IDs from the previous
         * physical snapshot, even if a different active record is corrupt. */
        uint8_t kept = 0;
        for (uint8_t i = 0; i < tp->data.points; ++i) {
            if (released[tp->data.coords[i].track_id]) continue;
            tp->data.coords[kept] = tp->data.coords[i];
            ctx->track_id[kept++] = tp->data.coords[i].track_id;
        }
        tp->data.points = kept;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    /* A valid release was received even when ACK transport reports failure.
     * Publish that physical snapshot before returning the error; a later
     * markerless poll must not resurrect the old held coordinate. */
    ESP_RETURN_ON_ERROR(err, TAG, "acknowledge report");
    return invalid_active ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

uint32_t cst9220_report_sequence(esp_lcd_touch_handle_t touch)
{
    if (!touch || touch->read_data != cst9220_read_data) return 0;
    const cst9220_t *ctx = (const cst9220_t *)touch;
    portENTER_CRITICAL(&touch->data.lock);
    const uint32_t sequence = ctx->report_sequence;
    portEXIT_CRITICAL(&touch->data.lock);
    return sequence;
}

static bool cst9220_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                           uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t count = tp->data.points;
    if (count > max_point_num) {
        count = max_point_num;
    }
    for (uint8_t i = 0; i < count; ++i) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    portEXIT_CRITICAL(&tp->data.lock);
    *point_num = count;
    return count > 0;
}

static esp_err_t cst9220_get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num)
{
    cst9220_t *ctx = (cst9220_t *)tp;
    ESP_RETURN_ON_FALSE(track_id && point_num <= CST9220_MAX_POINTS, ESP_ERR_INVALID_ARG, TAG, "invalid track ID request");
    portENTER_CRITICAL(&tp->data.lock);
    memcpy(track_id, ctx->track_id, point_num);
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static esp_err_t cst9220_enter_sleep(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_ERROR(cst9220_write_command((cst9220_t *)tp, 0xD105), TAG, "sleep touch");
    cst9220_clear_data(tp);
    return ESP_OK;
}

static esp_err_t cst9220_exit_sleep(esp_lcd_touch_handle_t tp)
{
    cst9220_t *ctx = (cst9220_t *)tp;
    cst9220_clear_data(tp);
    if (tp->config.rst_gpio_num == GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(cst9220_write_command(ctx, 0xD109), TAG, "wake touch");
        vTaskDelay(pdMS_TO_TICKS(10));
        if (ctx->legacy_protocol) return ESP_OK;
    } else ESP_RETURN_ON_ERROR(cst9220_reset(&tp->config), TAG, "reset on wake");
    return cst9220_enter_report_mode(ctx);
}

static esp_err_t cst9220_del(esp_lcd_touch_handle_t tp)
{
    cst9220_t *ctx = (cst9220_t *)tp;
    esp_err_t err = i2c_master_bus_rm_device(ctx->i2c_dev);
    if (err != ESP_OK) return err;
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    free(ctx);
    return err;
}

esp_err_t cst9220_new(i2c_master_bus_handle_t bus,
                      const esp_lcd_touch_config_t *config,
                      esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(bus && config && out_touch, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    cst9220_t *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no memory");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST9220_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &ctx->i2c_dev);
    if (ret != ESP_OK) {
        free(ctx);
        return ret;
    }

    ctx->base.config = *config;
    ctx->base.data.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    ctx->base.read_data = cst9220_read_data;
    ctx->base.get_xy = cst9220_get_xy;
    ctx->base.get_track_id = cst9220_get_track_id;
    ctx->base.enter_sleep = cst9220_enter_sleep;
    ctx->base.exit_sleep = cst9220_exit_sleep;
    ctx->base.del = cst9220_del;

    if (config->int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t irq_cfg = {
            .pin_bit_mask = 1ULL << config->int_gpio_num,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = config->levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&irq_cfg), fail, TAG, "configure interrupt");
    }
    ESP_GOTO_ON_ERROR(cst9220_reset(config), fail, TAG, "reset touch");

    ESP_GOTO_ON_ERROR(cst9220_write_command(ctx, 0xD101), fail, TAG, "enter information mode");
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t chip_data[4] = {0};
    ESP_GOTO_ON_ERROR(cst9220_read_command(ctx, 0xD204, chip_data, sizeof(chip_data)), fail, TAG, "read chip ID");
    const uint16_t chip_id = (uint16_t)(((uint16_t)chip_data[3] << 8) | chip_data[2]);
    ESP_GOTO_ON_FALSE(chip_id == CST9220_CHIP_ID, ESP_ERR_INVALID_RESPONSE, fail, TAG,
                      "unexpected chip ID 0x%04X", chip_id);
    ctx->project_id = (uint16_t)(((uint16_t)chip_data[1] << 8) | chip_data[0]);
    ctx->legacy_protocol = ctx->project_id == CST9220_PROJECT_LEGACY;
    /* Identification already selected D101. Only legacy firmware leaves it. */
    if (ctx->legacy_protocol)
        ESP_GOTO_ON_ERROR(cst9220_enter_report_mode(ctx), fail, TAG, "select report protocol");

    ESP_LOGI(TAG, "CST9220 detected at 0x%02X, project=0x%04X, protocol=%s, report_mode=%s, ACK=%s",
             CST9220_I2C_ADDRESS, ctx->project_id, ctx->legacy_protocol ? "legacy" : "current",
             ctx->legacy_protocol ? "D109" : "D101", ctx->legacy_protocol ? "none" : "D000=AB");
    *out_touch = &ctx->base;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(ctx->i2c_dev);
    free(ctx);
    return ret;
}
