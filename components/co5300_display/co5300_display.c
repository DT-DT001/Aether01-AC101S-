#include "co5300_display.h"
#include "co5300_te.h"

#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "esp_lcd_panel_interface.h"

static const char *TAG = "co5300_display";
static esp_err_t (*s_original_delete)(esp_lcd_panel_t *);
static esp_lcd_panel_io_handle_t s_panel_io;

static esp_err_t panel_delete(esp_lcd_panel_t *panel)
{
    esp_err_t error = co5300_te_deinit(s_panel_io);
    if (error != ESP_OK) return error;
    s_panel_io = NULL;
    return s_original_delete(panel);
}

/*
 * KD021WVOSN009-C003B is a native 480 x 480 panel. The upstream driver's
 * fallback sequence targets a smaller active window, so keep the module's
 * full address window in this project instead of depending on that fallback.
 * The remaining commands follow Espressif's CO5300 QSPI initialization.
 */
static const co5300_lcd_init_cmd_t kd021wvosn009_init[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0x28, NULL, 0, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 10},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x11, NULL, 0, 120},
};

esp_err_t co5300_display_new(const co5300_display_config_t *config,
                             esp_lcd_panel_io_handle_t *out_io,
                             esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_FALSE(config && out_io && out_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!s_panel_io, ESP_ERR_INVALID_STATE, TAG, "only one fitted panel is supported");

    const spi_bus_config_t bus_cfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        config->clk, config->data0, config->data1, config->data2, config->data3,
        config->max_transfer_bytes);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->host, &bus_cfg, SPI_DMA_CH_AUTO), TAG,
                        "initialize QSPI bus");

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(
        config->cs, config->on_color_done, config->user_ctx);
    io_cfg.pclk_hz = config->clock_hz;
    /* At 48MHz one setup cycle is 20.8ns, above the module's 10ns CS setup. */
    io_cfg.cs_ena_pretrans = 1;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)config->host, &io_cfg, &io);
    if (ret != ESP_OK) {
        spi_bus_free(config->host);
        return ret;
    }

    const co5300_vendor_config_t vendor_cfg = {
        .init_cmds = kd021wvosn009_init,
        .init_cmds_size = sizeof(kd021wvosn009_init) / sizeof(kd021wvosn009_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = config->reset,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ret = esp_lcd_new_panel_co5300(io, &panel_cfg, &panel);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(io);
        spi_bus_free(config->host);
        return ret;
    }

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), fail, TAG, "reset panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), fail, TAG, "initialize panel");
    const esp_err_t te_error = co5300_te_init(io, config->te);
    if (te_error != ESP_OK) ESP_LOGW(TAG, "TE synchronization unavailable: %s", esp_err_to_name(te_error));
    s_original_delete = panel->del;
    s_panel_io = io;
    panel->del = panel_delete;

    ESP_LOGI(TAG, "CO5300AF QSPI panel initialized: %ux%u at %lu Hz",
             config->width, config->height, (unsigned long)config->clock_hz);
    *out_io = io;
    *out_panel = panel;
    return ESP_OK;

fail:
    esp_lcd_panel_del(panel);
    esp_lcd_panel_io_del(io);
    spi_bus_free(config->host);
    return ret;
}
