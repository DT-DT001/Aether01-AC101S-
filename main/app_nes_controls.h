#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_NES_CONTROL_COUNT 11U
#define APP_NES_CONTROL_SLOTS 2U

typedef enum {
    APP_NES_CONTROL_UP, APP_NES_CONTROL_DOWN, APP_NES_CONTROL_LEFT, APP_NES_CONTROL_RIGHT,
    APP_NES_CONTROL_A, APP_NES_CONTROL_B, APP_NES_CONTROL_SELECT, APP_NES_CONTROL_START,
    APP_NES_CONTROL_PAUSE, APP_NES_CONTROL_RESET, APP_NES_CONTROL_EXIT,
} app_nes_control_t;
typedef struct {
    bool ready, pending, deferred;
    esp_err_t error;
    uint32_t revision;
    uint8_t keys[APP_NES_CONTROL_COUNT][APP_NES_CONTROL_SLOTS];
} app_nes_controls_snapshot_t;

/* Initializes defaults/validated NVS settings and an event-driven persistence
 * worker. No USB or GPIO work. Call after normal NVS initialization. */
esp_err_t app_nes_controls_init(void);
void app_nes_controls_set_persistence_guard(bool (*busy)(void *), void *context);
void app_nes_controls_get_snapshot(app_nes_controls_snapshot_t *snapshot);
/* Bounded, cache-only callback for aether_usb_host_config_t.keyboard_map. */
uint16_t app_nes_controls_map(void *unused, const uint8_t keys[32]);
/* usage0 unbinds. One physical key can belong to only one action/slot.
 * On conflicting assignment, conflict_action identifies the existing action.
 * Requests apply immediately in RAM; NVS writes run on the worker when idle. */
esp_err_t app_nes_controls_set(app_nes_control_t action, unsigned slot, uint8_t usage,
                                unsigned *conflict_action);
esp_err_t app_nes_controls_reset(void);
const char *app_nes_controls_action_name(unsigned action);
unsigned app_nes_controls_key_count(void);
const char *app_nes_controls_key_name(unsigned index);
uint8_t app_nes_controls_key_usage(unsigned index);
unsigned app_nes_controls_key_index(uint8_t usage);
