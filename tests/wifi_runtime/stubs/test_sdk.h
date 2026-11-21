#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef const char *esp_event_base_t;
extern esp_event_base_t WIFI_EVENT,IP_EVENT;
typedef void *esp_event_handler_instance_t;
typedef void (*event_handler_t)(void *,esp_event_base_t,int32_t,void *);
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_SCAN_DONE 3
#define IP_EVENT_STA_GOT_IP 4
#define IP_EVENT_STA_LOST_IP 5
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
typedef struct { unsigned value; } esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_netif_t *esp_netif; struct { esp_ip4_addr_t ip; } ip_info; } ip_event_got_ip_t;
typedef struct { uint16_t reason; } wifi_event_sta_disconnected_t;
typedef struct { uint32_t status; } wifi_event_sta_scan_done_t;
typedef enum { WIFI_AUTH_OPEN,WIFI_AUTH_WEP,WIFI_AUTH_WPA_PSK,WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,WIFI_AUTH_ENTERPRISE,WIFI_AUTH_WPA3_PSK,WIFI_AUTH_WPA2_WPA3_PSK } wifi_auth_mode_t;
#define WPA3_SAE_PWE_BOTH 2
#define WIFI_IF_STA 0
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 1
#define WIFI_SCAN_TYPE_ACTIVE 0
#define WIFI_PS_MIN_MODEM 1
typedef struct {
    int static_rx_buf_num,dynamic_rx_buf_num,tx_buf_type,static_tx_buf_num;
    int cache_tx_buf_num,rx_mgmt_buf_num,mgmt_sbuf_num,rx_ba_win;
} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){.static_rx_buf_num=10,.dynamic_rx_buf_num=32,.static_tx_buf_num=16,.cache_tx_buf_num=32,.rx_mgmt_buf_num=5,.mgmt_sbuf_num=32,.rx_ba_win=6})
typedef struct { uint8_t ssid[33],bssid[6],primary; int8_t rssi; wifi_auth_mode_t authmode; } wifi_ap_record_t;
typedef struct { bool show_hidden; int scan_type; } wifi_scan_config_t;
typedef struct { struct { uint8_t ssid[32],password[64],bssid[6],channel; bool bssid_set;
    struct { wifi_auth_mode_t authmode; } threshold;
    struct { bool capable,required; } pmf_cfg;
    int sae_pwe_h2e;
} sta; } wifi_config_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) ((unsigned)((p)->addr&255)),((unsigned)(((p)->addr>>8)&255)),((unsigned)(((p)->addr>>16)&255)),((unsigned)((p)->addr>>24))
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int32_t,event_handler_t,void *,esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t,int32_t,esp_event_handler_instance_t);
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(void *);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_set_config(int,wifi_config_t *);
esp_err_t esp_wifi_set_ps(int);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *,bool);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *,wifi_ap_record_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
