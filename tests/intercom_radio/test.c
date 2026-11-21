#include "aether_intercom_radio.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

#define ESP_ERR_ESPNOW_NO_MEM 0x3001
#include "radio-timing-types.inc"

typedef enum { WIRE_START = 5, WIRE_AUDIO, WIRE_STOP } wire_kind_t;
typedef struct {
    bool paired, have_pending_voice, sending_voice;
    int64_t last_pong, last_voice_at, tx_until, tx_retry_at, fast_rate_hold;
    uint64_t peer_epoch;
    unsigned tx_retries;
    queued_voice_t pending_voice;
} radio_worker_t;

static struct {
    atomic_uint tx_audio_expired, tx_submit_busy, tx_audio_acked, tx_mac_failed, tx_callback_timeout;
} s_metrics;
static atomic_int s_send_result;
static queued_voice_t s_tx_queue[TX_CAPACITY];
static unsigned s_tx_head, s_tx_count;
static unsigned attempts;
static wire_kind_t attempted_kind;
static uint16_t attempted_sequence;
static esp_err_t transmit_result;

static bool take_voice(queued_voice_t *packet)
{
    if (!s_tx_count) return false;
    *packet = s_tx_queue[s_tx_head];
    s_tx_head = (s_tx_head + 1) % TX_CAPACITY;
    --s_tx_count;
    return true;
}

static esp_err_t send_authenticated(radio_worker_t *worker, wire_kind_t kind, uint64_t epoch,
                                     uint16_t session, uint16_t sequence,
                                     const void *payload, size_t length, int64_t now)
{
    assert(epoch == 7 && session == 3 && payload);
    assert(length == (kind == WIRE_AUDIO ? 1U : 0U));
    ++attempts;
    attempted_kind = kind;
    attempted_sequence = sequence;
    if (transmit_result == ESP_OK) {
        worker->tx_until = now + 150000;
        atomic_store(&s_send_result, TX_WAITING);
    }
    return transmit_result;
}

#include "radio-timing-production.inc"

static void enqueue(aether_intercom_packet_kind_t kind, uint16_t sequence, int64_t now)
{
    assert(s_tx_count < TX_CAPACITY);
    unsigned tail = (s_tx_head + s_tx_count++) % TX_CAPACITY;
    s_tx_queue[tail] = (queued_voice_t){
        .packet = {.kind = kind, .session = 3, .sequence = sequence, .length = kind == AETHER_INTERCOM_PACKET_AUDIO},
        .queued_us = now,
    };
}

int main(void)
{
    radio_worker_t worker = {.paired = true, .last_pong = 7, .peer_epoch = 7};
    const int64_t now = 100000;
    s_tx_head = TX_CAPACITY - 1;
    enqueue(AETHER_INTERCOM_PACKET_START, 0, 1);
    enqueue(AETHER_INTERCOM_PACKET_AUDIO, 1, 1);
    enqueue(AETHER_INTERCOM_PACKET_AUDIO, 2, 2);
    enqueue(AETHER_INTERCOM_PACKET_AUDIO, 3, now - 1000);
    enqueue(AETHER_INTERCOM_PACKET_STOP, 4, 3);
    remove_expired_queued_audio(now);
    assert(s_tx_count == 4 && s_tx_queue[s_tx_head].packet.kind == AETHER_INTERCOM_PACKET_START);
    assert(service_voice_tx(&worker, now) && attempted_kind == WIRE_START);
    assert(!finish_tx(&worker, now + 500) && attempts == 1);
    atomic_store(&s_send_result, TX_ACKED);
    assert(finish_tx(&worker, now + 1000));

    transmit_result = ESP_ERR_ESPNOW_NO_MEM;
    assert(service_voice_tx(&worker, now + 1000));
    assert(attempted_sequence == 3 && attempted_kind == WIRE_AUDIO && attempts == 2);
    assert(atomic_load(&s_metrics.tx_audio_expired) == 2);
    assert(!service_voice_tx(&worker, now + 2999) && attempts == 2);
    transmit_result = ESP_OK;
    assert(service_voice_tx(&worker, now + 3000) && attempts == 3);
    atomic_store(&s_send_result, TX_ACKED);
    assert(finish_tx(&worker, now + 4000) && atomic_load(&s_metrics.tx_audio_acked) == 1);
    assert(service_voice_tx(&worker, now + 4000) && attempted_kind == WIRE_STOP);
    atomic_store(&s_send_result, TX_ACKED);
    assert(finish_tx(&worker, now + 5000) && !s_tx_count);

    enqueue(AETHER_INTERCOM_PACKET_AUDIO, 5, now + 5000);
    assert(service_voice_tx(&worker, now + 5000));
    atomic_store(&s_send_result, TX_FAILED);
    assert(!finish_tx(&worker, now + 6000));
    assert(worker.fast_rate_hold > now && worker.tx_retry_at == now + 8000);
    assert(service_voice_tx(&worker, now + 8000) && attempted_sequence == 5);
    atomic_store(&s_send_result, TX_FAILED);
    assert(finish_tx(&worker, now + 9000) && !worker.have_pending_voice);

    enqueue(AETHER_INTERCOM_PACKET_AUDIO, 6, now + 10000);
    assert(service_voice_tx(&worker, now + 10000));
    assert(finish_tx(&worker, worker.tx_until) && !worker.have_pending_voice);
    assert(atomic_load(&s_metrics.tx_callback_timeout) == 1);
    assert(atomic_load(&s_metrics.tx_mac_failed) == 2 && atomic_load(&s_metrics.tx_submit_busy) == 1);
    puts("PASS radio voice timing: stale audio expires, control stays ordered, transient send retries stay bounded");
    return 0;
}
