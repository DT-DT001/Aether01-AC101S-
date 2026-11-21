#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AETHER_WIFI_MAX_APS 16
typedef struct aether_wifi *aether_wifi_handle_t;
typedef enum { AETHER_WIFI_OFF,AETHER_WIFI_STARTING,AETHER_WIFI_IDLE,
    AETHER_WIFI_SCANNING,AETHER_WIFI_CONNECTING,AETHER_WIFI_CONNECTED,
    AETHER_WIFI_DISCONNECTING,AETHER_WIFI_ERROR } aether_wifi_state_t;
typedef struct {
    char ssid[33];
    uint8_t bssid[6],channel,authmode;
    int8_t rssi;
    bool password_required,supported;
} aether_wifi_ap_t;
typedef struct {
    aether_wifi_state_t state;
    bool enabled,connected,scanning,saved_network,persistence_pending;
    uint8_t ap_count,retry_count;
    uint16_t disconnect_reason;
    uint32_t scan_generation,retry_delay_ms;
    esp_err_t last_error,storage_error;
    char ssid[33],ip[16];
    int8_t rssi;
    bool peer_radio, enabled_preference;
} aether_wifi_status_t;
/** One station service; NVS is initialized by main. Wireless starts OFF.
 * All commands enqueue without waiting for scans, association or NVS writes. */
esp_err_t aether_wifi_new(aether_wifi_handle_t *out);
aether_wifi_handle_t aether_wifi_get_default(void);
/** Defer credential NVS writes/erase while media owns Flash bandwidth. */
esp_err_t aether_wifi_set_persistence_guard(aether_wifi_handle_t,bool (*busy)(void *),void *context);
esp_err_t aether_wifi_set_enabled(aether_wifi_handle_t,bool);
esp_err_t aether_wifi_scan(aether_wifi_handle_t);
esp_err_t aether_wifi_connect(aether_wifi_handle_t,const aether_wifi_ap_t *,const char *password,bool save);
esp_err_t aether_wifi_disconnect(aether_wifi_handle_t);
esp_err_t aether_wifi_forget(aether_wifi_handle_t);
/** Temporarily give a connectionless client exclusive use of the station.
 * The owner task suspends association/scans and restores the enabled preference
 * on release. Only one client may acquire the station. Release is idempotent. */
esp_err_t aether_wifi_peer_acquire(aether_wifi_handle_t, uint32_t timeout_ms);
esp_err_t aether_wifi_peer_release(aether_wifi_handle_t, uint32_t timeout_ms);
esp_err_t aether_wifi_get_status(aether_wifi_handle_t,aether_wifi_status_t *);
esp_err_t aether_wifi_get_ap(aether_wifi_handle_t,size_t,aether_wifi_ap_t *);
const char *aether_wifi_security_name(uint8_t authmode);
esp_err_t aether_wifi_del(aether_wifi_handle_t,uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
