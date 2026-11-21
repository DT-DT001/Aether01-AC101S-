#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_INTERCOM_MAX_PAYLOAD 224U

typedef enum {
    AETHER_INTERCOM_RADIO_STOPPED,
    AETHER_INTERCOM_RADIO_STARTING,
    AETHER_INTERCOM_RADIO_READY,
    AETHER_INTERCOM_RADIO_SEARCHING,
    AETHER_INTERCOM_RADIO_PAIRED,
    AETHER_INTERCOM_RADIO_ERROR,
} aether_intercom_radio_state_t;

typedef struct {
    aether_intercom_radio_state_t state;
    bool peer_online;
    bool local_has_priority;
    uint8_t peer_mac[6];
    int8_t rssi;
    esp_err_t last_error;
} aether_intercom_radio_status_t;

typedef enum {
    AETHER_INTERCOM_PACKET_START,
    AETHER_INTERCOM_PACKET_AUDIO,
    AETHER_INTERCOM_PACKET_STOP,
} aether_intercom_packet_kind_t;

typedef struct {
    aether_intercom_packet_kind_t kind;
    uint16_t session;
    uint16_t sequence;
    uint16_t length;
    uint8_t data[AETHER_INTERCOM_MAX_PAYLOAD];
} aether_intercom_packet_t;

typedef struct {
    uint32_t tx_audio_acked;
    uint32_t tx_queue_dropped;
    uint32_t tx_audio_expired;
    uint32_t tx_submit_busy;
    uint32_t tx_mac_failed;
    uint32_t tx_callback_timeout;
    uint32_t rx_audio_queued;
    uint32_t rx_wire_dropped;
    uint32_t rx_voice_dropped;
} aether_intercom_radio_metrics_t;

/** Starts the connectionless Wi-Fi station on a dedicated channel. */
esp_err_t aether_intercom_radio_start(void);
/** A zero timeout requests shutdown. A positive timeout also waits for teardown. */
esp_err_t aether_intercom_radio_stop(uint32_t timeout_ms);
/** Both unpaired units must search. Each search runs for up to 30 seconds. */
esp_err_t aether_intercom_radio_search(void);
esp_err_t aether_intercom_radio_forget(void);
void aether_intercom_radio_get_status(aether_intercom_radio_status_t *out);
/** Cumulative counters since this radio session started; safe from any task. */
void aether_intercom_radio_get_metrics(aether_intercom_radio_metrics_t *out);

/** Queues one packet without blocking. START opens a talk session only on
 * success. AUDIO uses increasing nonzero sequence numbers and can be dropped
 * when the queue fills. STOP closes the talk session even if queuing fails. */
esp_err_t aether_intercom_radio_send(aether_intercom_packet_kind_t kind,
                                  const void *data, size_t size);
bool aether_intercom_radio_receive(aether_intercom_packet_t *out, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
