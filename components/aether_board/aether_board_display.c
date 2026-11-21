#include "aether_board.h"
#include "aether_board_pins.h"
#include "co5300_display.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_display";

esp_err_t aether_board_set_display_sleep(aether_board_t *board, bool sleeping)
{
    ESP_RETURN_ON_FALSE(board && board->display_ready && board->display,
                        ESP_ERR_INVALID_STATE, TAG, "display not initialized");
    if (!sleeping && !board->display_sleeping && !board->display_needs_reinit) {
        return ESP_OK;
    }

    if (sleeping) {
        if (board->display_sleeping) return ESP_OK;
        const esp_err_t err = co5300_display_set_sleep(board->display_io, true);
        /* A failed command may already have blanked the panel. Keep the
         * supplies stable, suppress drawing, and recover on the next wake. */
        board->display_sleeping = true;
        board->display_needs_reinit = err != ESP_OK;
        if (err == ESP_OK) ESP_LOGI(TAG, "display asleep (DCS 0x10); CLDO3/AVDD_EN and touch retained");
        return err;
    }

    esp_err_t err = ESP_OK;
    if (!board->display_needs_reinit) {
        err = co5300_display_set_sleep(board->display_io, false);
        if (err == ESP_OK) err = esp_lcd_panel_co5300_set_brightness(board->display, board->display_brightness);
        if (err == ESP_OK) {
            board->display_sleeping = false;
            ESP_LOGI(TAG, "display awake (DCS 0x11/0x29), brightness=%u%%", board->display_brightness);
            return ESP_OK;
        }
        board->display_needs_reinit = true;
        ESP_LOGW(TAG, "ordinary display wake failed: %s; attempting cold recovery", esp_err_to_name(err));
    }

    err = tg29_set_rail_enabled(board->pmic, TG29_RAIL_CLDO3, true);
    if (err != ESP_OK) goto wake_failed;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(AETHER_GPIO_LCD_AVDD_EN, 1);
    if (err != ESP_OK) goto wake_failed;
    vTaskDelay(pdMS_TO_TICKS(20));
    /* A partially completed DCS command leaves state unknown. Hardware reset
     * also resets the driver's cached deep/sleep state before reinitializing. */
    err = esp_lcd_panel_reset(board->display);
    if (err == ESP_OK) err = esp_lcd_panel_init(board->display);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(board->display, true);
    if (err == ESP_OK) err = esp_lcd_panel_co5300_set_brightness(board->display, board->display_brightness);
    if (err != ESP_OK) goto wake_failed;
    board->display_sleeping = false;
    board->display_needs_reinit = false;
    ESP_LOGI(TAG, "display cold recovery completed");
    return ESP_OK;

wake_failed:
    gpio_set_level(AETHER_GPIO_LCD_AVDD_EN, 0);
    gpio_set_level(AETHER_GPIO_LCD_RST, 0);
    board->display_sleeping = true;
    board->display_needs_reinit = true;
    const esp_err_t rollback = tg29_set_rail_enabled(board->pmic, TG29_RAIL_CLDO3, false);
    if (rollback != ESP_OK) ESP_LOGW(TAG, "display wake rollback rail: %s", esp_err_to_name(rollback));
    return err;
}

esp_err_t aether_board_set_display_brightness(aether_board_t *board,
                                              uint8_t percent)
{
    ESP_RETURN_ON_FALSE(board && board->display_ready && board->display,
                        ESP_ERR_INVALID_STATE, TAG, "display not initialized");
    ESP_RETURN_ON_FALSE(percent <= 100, ESP_ERR_INVALID_ARG, TAG,
                        "brightness must be 0..100");
    board->display_brightness = percent;
    if (board->display_sleeping) {
        return ESP_OK;
    }
    return esp_lcd_panel_co5300_set_brightness(board->display, percent);
}

esp_err_t aether_board_set_touch_sleep(aether_board_t *board, bool sleeping)
{
    ESP_RETURN_ON_FALSE(board && board->touch_ready && board->touch,
                        ESP_ERR_INVALID_STATE, TAG, "touch not initialized");
    if (board->touch_sleeping == sleeping) {
        return ESP_OK;
    }
    esp_err_t err = sleeping ? esp_lcd_touch_enter_sleep(board->touch)
                             : esp_lcd_touch_exit_sleep(board->touch);
    if (err == ESP_OK) {
        board->touch_sleeping = sleeping;
    }
    return err;
}
