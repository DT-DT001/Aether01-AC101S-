#include "aether_intercom_radio.h"
#include "aether_wifi.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <string.h>

#define CHANNEL 6
#define PAIR_RSSI (-48)
#define PAIR_SAMPLES 3U
#define RX_CAPACITY 12U
#define REPLAY_WINDOW 8U
#define TX_AUDIO_CAPACITY 6U
#define TX_CAPACITY (TX_AUDIO_CAPACITY + 2U)
#define HEADER_SIZE 16U
#define TAG_SIZE 8U
#define PUBLIC_KEY_SIZE 32U
#define MAC_SIZE ESP_NOW_ETH_ALEN
#define VOICE_MAX_WIRE (HEADER_SIZE + AETHER_INTERCOM_MAX_PAYLOAD + TAG_SIZE)
#define HELLO_SIZE (4U + 8U + PUBLIC_KEY_SIZE)
#define OFFER_SIZE (4U + 8U + 8U + MAC_SIZE)
#define PAIR_DURATION_US 30000000LL
#define CANDIDATE_FRESH_US 900000LL
#define TENTATIVE_DURATION_US 3500000LL
#define PEER_OFFLINE_US 4000000LL
#define HEARTBEAT_US 1100000LL
#define TX_TIMEOUT_US 150000LL
#define TX_AUDIO_MAX_AGE_US 55000LL
#define TX_AUDIO_RETRY_AGE_US 28000LL
#define TX_RETRY_PAUSE_US 2000LL
#define LINK_STATS_INTERVAL_US 2000000LL
#define FAST_RATE_REENTER_DBM (-64)
#define FAST_RATE_LEAVE_DBM (-72)
#define FAST_RATE_RETRY_US 3000000LL
#define MAC_ALGORITHM PSA_ALG_TRUNCATED_MAC(PSA_ALG_HMAC(PSA_ALG_SHA_256), TAG_SIZE)

_Static_assert(VOICE_MAX_WIRE <= ESP_NOW_MAX_DATA_LEN, "Packet must fit ESP-NOW v1");

typedef enum {
    WIRE_HELLO = 1, WIRE_OFFER, WIRE_PING, WIRE_PONG,
    WIRE_START, WIRE_AUDIO, WIRE_STOP,
} wire_kind_t;

typedef struct {
    uint8_t bytes[VOICE_MAX_WIRE];
    uint8_t source[MAC_SIZE];
    uint16_t length;
    int8_t rssi;
} wire_packet_t;

typedef struct {
    aether_intercom_packet_t packet;
    int64_t queued_us;
} queued_voice_t;

typedef enum { TX_WAITING, TX_ACKED, TX_FAILED } tx_result_t;

typedef struct {
    uint8_t version;
    uint8_t mac[MAC_SIZE];
    uint8_t secret[32];
} pair_record_t;

typedef struct {
    uint64_t nonce;
    uint8_t mac[MAC_SIZE];
    uint8_t public_key[PUBLIC_KEY_SIZE];
    unsigned samples;
    int8_t best_rssi;
    int64_t last_seen;
} candidate_t;

typedef struct {
    mbedtls_svc_key_id_t private_key;
    mbedtls_svc_key_id_t auth_key;
    uint8_t local_mac[MAC_SIZE];
    uint8_t public_key[PUBLIC_KEY_SIZE];
    uint64_t local_epoch;
    uint64_t peer_epoch;
    uint64_t pair_nonce;
    uint64_t ping_challenge;
    uint64_t pong_epoch;
    uint64_t pong_challenge;
    pair_record_t peer;
    candidate_t candidate;
    bool paired;
    bool link_installed;
    bool received_talk;
    bool received_stop;
    uint16_t rx_session;
    uint16_t rx_sequence;
    uint8_t rx_seen;
    int64_t search_until;
    int64_t tentative_until;
    int64_t next_hello;
    int64_t next_offer;
    int64_t next_ping;
    int64_t last_pong;
    int64_t tx_until;
    int64_t tx_retry_at;
    int64_t fast_rate_hold;
    int64_t next_metrics;
    int64_t last_voice_at;
    unsigned tx_retries;
    bool have_pending_voice;
    bool sending_voice;
    bool fast_rate;
    bool rate_unavailable;
    int8_t last_rssi;
    queued_voice_t pending_voice;
    aether_intercom_radio_metrics_t previous_metrics;
} radio_worker_t;

static const char *TAG = "intercom_radio";
static const uint8_t broadcast[MAC_SIZE] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const char nvs_namespace[] = "aether_intercom";
static const char nvs_key[] = "peer";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static aether_intercom_radio_status_t s_status;
static bool s_running;
static bool s_stop_requested;
static bool s_search_requested;
static bool s_forget_requested;
static bool s_tx_open;
static uint16_t s_tx_session;
static uint16_t s_tx_sequence;
static unsigned s_tx_head, s_tx_count;
static atomic_int s_send_result;
static struct {
    atomic_uint tx_audio_acked, tx_queue_dropped, tx_audio_expired, tx_submit_busy;
    atomic_uint tx_mac_failed, tx_callback_timeout, rx_audio_queued, rx_wire_dropped, rx_voice_dropped;
} s_metrics;
static StaticQueue_t s_wire_queue_storage, s_voice_queue_storage;
static QueueHandle_t s_wire_queue, s_voice_queue;
static StaticSemaphore_t s_worker_wake_storage;
static _Atomic(SemaphoreHandle_t) s_worker_wake;
/* Retained queue controls let close safely race a consumer leaving receive(). */
static EXT_RAM_BSS_ATTR uint8_t s_wire_queue_data[RX_CAPACITY * sizeof(wire_packet_t)];
static EXT_RAM_BSS_ATTR uint8_t s_voice_queue_data[RX_CAPACITY * sizeof(aether_intercom_packet_t)];
static EXT_RAM_BSS_ATTR queued_voice_t s_tx_queue[TX_CAPACITY];

static void wake_worker(void)
{
    /* Queue and wake storage survive opening/closing the page. The worker can
     * safely exit while a callback or GUI command is about to signal it. */
    SemaphoreHandle_t wake = atomic_load(&s_worker_wake);
    if (wake) xSemaphoreGive(wake);
}

static bool voice_expired(const queued_voice_t *entry, int64_t now, int64_t max_age)
{
    return entry->packet.kind == AETHER_INTERCOM_PACKET_AUDIO &&
           now - entry->queued_us >= max_age;
}

static void reset_metrics(void)
{
    atomic_store(&s_metrics.tx_audio_acked, 0);
    atomic_store(&s_metrics.tx_queue_dropped, 0);
    atomic_store(&s_metrics.tx_audio_expired, 0);
    atomic_store(&s_metrics.tx_submit_busy, 0);
    atomic_store(&s_metrics.tx_mac_failed, 0);
    atomic_store(&s_metrics.tx_callback_timeout, 0);
    atomic_store(&s_metrics.rx_audio_queued, 0);
    atomic_store(&s_metrics.rx_wire_dropped, 0);
    atomic_store(&s_metrics.rx_voice_dropped, 0);
}

/* Called with the outgoing lock held, and only when a producer finds a full
 * queue. Preserve START, STOP and any remaining audio in sequencing order. */
static void remove_expired_queued_audio(int64_t now)
{
    for (unsigned i = 0; i < s_tx_count; ++i) {
        unsigned index = (s_tx_head + i) % TX_CAPACITY;
        if (!voice_expired(&s_tx_queue[index], now, TX_AUDIO_MAX_AGE_US)) continue;
        for (unsigned j = i; j + 1 < s_tx_count; ++j) {
            unsigned next = (index + 1) % TX_CAPACITY;
            s_tx_queue[index] = s_tx_queue[next];
            index = next;
        }
        --s_tx_count;
        atomic_fetch_add(&s_metrics.tx_audio_expired, 1);
        return;
    }
}

static void wipe(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size--) *bytes++ = 0;
}

static uint64_t random_epoch(void)
{
    uint64_t value;
    do {
        esp_fill_random(&value, sizeof(value));
    } while (!value);
    return value;
}

static uint64_t get_u64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (i * 8U);
    return value;
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (i * 8U));
}

static uint16_t get_u16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void wire_prefix(uint8_t *bytes, wire_kind_t kind)
{
    bytes[0] = 'A';
    bytes[1] = 'E';
    bytes[2] = 1;
    bytes[3] = (uint8_t)kind;
}

static void reset_outgoing(void)
{
    s_tx_head = s_tx_count = 0;
    s_tx_open = false;
}

static void publish(radio_worker_t *worker, aether_intercom_radio_state_t state, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.last_error = error;
    s_status.peer_online = state == AETHER_INTERCOM_RADIO_PAIRED && worker->last_pong != 0;
    if (worker->paired) {
        memcpy(s_status.peer_mac, worker->peer.mac, MAC_SIZE);
        s_status.local_has_priority = memcmp(worker->local_mac, worker->peer.mac, MAC_SIZE) < 0;
    } else {
        memset(s_status.peer_mac, 0, MAC_SIZE);
        s_status.local_has_priority = false;
    }
    if (!s_status.peer_online) {
        s_status.rssi = 0;
        reset_outgoing();
    }
    portEXIT_CRITICAL(&s_lock);
}

static void recv_callback(const esp_now_recv_info_t *info, const uint8_t *bytes, int length)
{
    if (!info || !info->src_addr || !info->des_addr || !info->rx_ctrl || !bytes ||
        length < 4 || (unsigned)length > VOICE_MAX_WIRE || bytes[0] != 'A' ||
        bytes[1] != 'E' || bytes[2] != 1 || bytes[3] < WIRE_HELLO ||
        bytes[3] > WIRE_STOP) return;
    bool is_broadcast = memcmp(info->des_addr, broadcast, MAC_SIZE) == 0;
    if (is_broadcast != (bytes[3] == WIRE_HELLO || bytes[3] == WIRE_OFFER)) return;
    wire_packet_t packet = {.length = (uint16_t)length, .rssi = info->rx_ctrl->rssi};
    memcpy(packet.source, info->src_addr, MAC_SIZE);
    memcpy(packet.bytes, bytes, (size_t)length);
    if (xQueueSend(s_wire_queue, &packet, 0) == pdTRUE) wake_worker();
    else if (bytes[3] == WIRE_AUDIO) atomic_fetch_add(&s_metrics.rx_wire_dropped, 1);
}

static void send_callback(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    atomic_store(&s_send_result, status == ESP_NOW_SEND_SUCCESS ? TX_ACKED : TX_FAILED);
    wake_worker();
}

static esp_err_t transmit(radio_worker_t *worker, const uint8_t *address,
                          const uint8_t *bytes, size_t length, int64_t now)
{
    atomic_store(&s_send_result, TX_WAITING);
    esp_err_t error = esp_now_send(address, bytes, length);
    if (error == ESP_OK) worker->tx_until = now + TX_TIMEOUT_US;
    return error;
}

static bool newer_u16(uint16_t value, uint16_t previous)
{
    uint16_t delta = (uint16_t)(value - previous);
    return delta && delta < 0x8000U;
}

static esp_err_t store_peer(const pair_record_t *peer)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(nvs_namespace, NVS_READWRITE, &nvs);
    if (error != ESP_OK) return error;
    error = peer ? nvs_set_blob(nvs, nvs_key, peer, sizeof(*peer)) : nvs_erase_key(nvs, nvs_key);
    if (error == ESP_ERR_NVS_NOT_FOUND && !peer) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(nvs);
    nvs_close(nvs);
    return error;
}

static esp_err_t load_peer(pair_record_t *peer)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(nvs_namespace, NVS_READONLY, &nvs);
    if (error != ESP_OK) return error;
    size_t size = sizeof(*peer);
    error = nvs_get_blob(nvs, nvs_key, peer, &size);
    nvs_close(nvs);
    if (error == ESP_OK && (size != sizeof(*peer) || peer->version != 1 || (peer->mac[0] & 1U))) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return error;
}

static esp_err_t install_peer(radio_worker_t *worker)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes, MAC_ALGORITHM);
    psa_status_t status = psa_import_key(&attributes, worker->peer.secret + ESP_NOW_KEY_LEN,
                                        ESP_NOW_KEY_LEN, &worker->auth_key);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) return ESP_FAIL;

    esp_now_peer_info_t peer = {.channel = CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = true};
    memcpy(peer.peer_addr, worker->peer.mac, MAC_SIZE);
    memcpy(peer.lmk, worker->peer.secret, sizeof(peer.lmk));
    esp_err_t error = esp_now_add_peer(&peer);
    wipe(&peer, sizeof(peer));
    if (error == ESP_OK) worker->link_installed = true;
    else {
        psa_destroy_key(worker->auth_key);
        worker->auth_key = MBEDTLS_SVC_KEY_ID_INIT;
    }
    return error;
}

static void remove_peer(radio_worker_t *worker)
{
    if (worker->link_installed) {
        esp_now_del_peer(worker->peer.mac);
        psa_destroy_key(worker->auth_key);
        worker->auth_key = MBEDTLS_SVC_KEY_ID_INIT;
    }
    worker->link_installed = worker->paired = worker->received_talk = false;
    worker->have_pending_voice = false;
    worker->sending_voice = false;
    worker->fast_rate = worker->rate_unavailable = false;
    worker->fast_rate_hold = 0;
    worker->pong_epoch = worker->ping_challenge = worker->peer_epoch = 0;
    worker->last_pong = worker->tentative_until = 0;
    wipe(&worker->peer, sizeof(worker->peer));
}

static void close_search(radio_worker_t *worker)
{
    if (worker->search_until) psa_destroy_key(worker->private_key);
    worker->private_key = MBEDTLS_SVC_KEY_ID_INIT;
    worker->search_until = 0;
    worker->candidate = (candidate_t){0};
}

static esp_err_t open_search(radio_worker_t *worker, int64_t now)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attributes, 255);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    psa_status_t status = psa_generate_key(&attributes, &worker->private_key);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ESP_LOGW(TAG, "pair key generation failed (%ld)", (long)status);
        return ESP_FAIL;
    }
    worker->search_until = now + PAIR_DURATION_US;
    size_t exported;
    status = psa_export_public_key(worker->private_key, worker->public_key,
                                   sizeof(worker->public_key), &exported);
    if (status != PSA_SUCCESS || exported != sizeof(worker->public_key)) {
        close_search(worker);
        return ESP_FAIL;
    }
    worker->pair_nonce = random_epoch();
    worker->next_hello = worker->next_offer = 0;
    return ESP_OK;
}

static esp_err_t install_candidate(radio_worker_t *worker, int64_t now)
{
    candidate_t *candidate = &worker->candidate;
    /* Derive independent link/authentication keys. The same sorted transcript
     * binds the ephemeral exchange to both opted-in devices and search windows. */
    uint8_t input[32 + 2 * (MAC_SIZE + 8) + 17];
    size_t agreed, hashed;
    psa_status_t status = psa_raw_key_agreement(PSA_ALG_ECDH, worker->private_key,
                                                candidate->public_key, PUBLIC_KEY_SIZE,
                                                input, 32, &agreed);
    if (status == PSA_SUCCESS && agreed == 32) {
        bool local_first = memcmp(worker->local_mac, candidate->mac, MAC_SIZE) < 0;
        memcpy(input + 32, local_first ? worker->local_mac : candidate->mac, MAC_SIZE);
        memcpy(input + 32 + MAC_SIZE, local_first ? candidate->mac : worker->local_mac, MAC_SIZE);
        put_u64(input + 32 + 2 * MAC_SIZE, local_first ? worker->pair_nonce : candidate->nonce);
        put_u64(input + 40 + 2 * MAC_SIZE, local_first ? candidate->nonce : worker->pair_nonce);
        memcpy(input + 48 + 2 * MAC_SIZE, "AetherIntercom-v1", 17);
        status = psa_hash_compute(PSA_ALG_SHA_256, input, sizeof(input), worker->peer.secret,
                                  sizeof(worker->peer.secret), &hashed);
    }
    wipe(input, sizeof(input));
    if (status != PSA_SUCCESS || agreed != 32 || hashed != sizeof(worker->peer.secret)) {
        wipe(&worker->peer, sizeof(worker->peer));
        return ESP_FAIL;
    }
    worker->peer.version = 1;
    memcpy(worker->peer.mac, candidate->mac, MAC_SIZE);
    esp_err_t error = install_peer(worker);
    if (error == ESP_OK) {
        worker->tentative_until = now + TENTATIVE_DURATION_US;
        worker->next_ping = now;
    } else wipe(&worker->peer, sizeof(worker->peer));
    return error;
}

static bool candidate_ready(const radio_worker_t *worker, int64_t now)
{
    return worker->candidate.samples >= PAIR_SAMPLES &&
           now - worker->candidate.last_seen <= CANDIDATE_FRESH_US;
}

static void handle_discovery(radio_worker_t *worker, const wire_packet_t *packet, int64_t now)
{
    if (!worker->search_until || packet->rssi < PAIR_RSSI ||
        !memcmp(packet->source, worker->local_mac, MAC_SIZE) || (packet->source[0] & 1U)) return;
    const uint8_t *bytes = packet->bytes;
    candidate_t *candidate = &worker->candidate;
    uint64_t nonce = packet->length >= 12 ? get_u64(bytes + 4) : 0;
    if (!nonce) return;

    if (bytes[3] == WIRE_HELLO && packet->length == HELLO_SIZE) {
        if (worker->link_installed && memcmp(packet->source, worker->peer.mac, MAC_SIZE)) return;
        bool same_peer = !memcmp(packet->source, candidate->mac, MAC_SIZE);
        if (!same_peer && candidate->samples && now - candidate->last_seen <= CANDIDATE_FRESH_US &&
            packet->rssi < candidate->best_rssi + 6) return;
        if (!same_peer || candidate->nonce != nonce || now - candidate->last_seen > CANDIDATE_FRESH_US) {
            *candidate = (candidate_t){.nonce = nonce, .best_rssi = packet->rssi};
            memcpy(candidate->mac, packet->source, MAC_SIZE);
            memcpy(candidate->public_key, bytes + 12, PUBLIC_KEY_SIZE);
        }
        if (memcmp(candidate->public_key, bytes + 12, PUBLIC_KEY_SIZE)) return;
        if (candidate->samples < PAIR_SAMPLES) ++candidate->samples;
        if (packet->rssi > candidate->best_rssi) candidate->best_rssi = packet->rssi;
        candidate->last_seen = now;
    } else if (bytes[3] == WIRE_OFFER && packet->length == OFFER_SIZE &&
               !worker->link_installed && candidate_ready(worker, now) &&
               !memcmp(packet->source, candidate->mac, MAC_SIZE) &&
               memcmp(packet->source, worker->local_mac, MAC_SIZE) < 0 &&
               candidate->nonce == nonce && get_u64(bytes + 12) == worker->pair_nonce &&
               !memcmp(bytes + 20, worker->local_mac, MAC_SIZE)) {
        esp_err_t error = install_candidate(worker, now);
        if (error != ESP_OK) publish(worker, AETHER_INTERCOM_RADIO_SEARCHING, error);
    }
}

static esp_err_t send_authenticated(radio_worker_t *worker, wire_kind_t kind, uint64_t target_epoch,
                                    uint16_t session, uint16_t sequence,
                                    const void *payload, size_t length, int64_t now)
{
    /* Put the direction and sender epoch into the authenticated context.
     * HELLO and OFFER never carry speech or a secret key. */
    uint8_t message[2 * MAC_SIZE + 8 + VOICE_MAX_WIRE];
    uint8_t *bytes = message + 2 * MAC_SIZE + 8;
    memcpy(message, worker->local_mac, MAC_SIZE);
    memcpy(message + MAC_SIZE, worker->peer.mac, MAC_SIZE);
    put_u64(message + 2 * MAC_SIZE, kind >= WIRE_START ? worker->local_epoch : 0);
    wire_prefix(bytes, kind);
    put_u64(bytes + 4, target_epoch);
    put_u16(bytes + 12, session);
    put_u16(bytes + 14, sequence);
    if (length) memcpy(bytes + HEADER_SIZE, payload, length);
    size_t mac_size;
    size_t useful = HEADER_SIZE + length;
    psa_status_t status = psa_mac_compute(worker->auth_key, MAC_ALGORITHM, message,
                                          2 * MAC_SIZE + 8 + useful, bytes + useful,
                                          TAG_SIZE, &mac_size);
    if (status != PSA_SUCCESS || mac_size != TAG_SIZE) return ESP_FAIL;
    return transmit(worker, worker->peer.mac, bytes, useful + TAG_SIZE, now);
}

static bool authenticated(const radio_worker_t *worker, const wire_packet_t *packet)
{
    if (packet->length < HEADER_SIZE + TAG_SIZE || !worker->link_installed ||
        memcmp(packet->source, worker->peer.mac, MAC_SIZE)) return false;
    uint8_t message[2 * MAC_SIZE + 8 + HEADER_SIZE + AETHER_INTERCOM_MAX_PAYLOAD];
    size_t bytes = packet->length - TAG_SIZE;
    memcpy(message, packet->source, MAC_SIZE);
    memcpy(message + MAC_SIZE, worker->local_mac, MAC_SIZE);
    put_u64(message + 2 * MAC_SIZE, packet->bytes[3] >= WIRE_START ? worker->peer_epoch : 0);
    memcpy(message + 2 * MAC_SIZE + 8, packet->bytes, bytes);
    return psa_mac_verify(worker->auth_key, MAC_ALGORITHM, message, 2 * MAC_SIZE + 8 + bytes,
                          packet->bytes + bytes, TAG_SIZE) == PSA_SUCCESS;
}

static void handle_voice(radio_worker_t *worker, const uint8_t *bytes, size_t length)
{
    if (!worker->paired || !worker->last_pong || !worker->peer_epoch) return;
    wire_kind_t wire_kind = (wire_kind_t)bytes[3];
    aether_intercom_packet_t packet = {
        .kind = (aether_intercom_packet_kind_t)(wire_kind - WIRE_START),
        .session = get_u16(bytes + 12), .sequence = get_u16(bytes + 14),
        .length = (uint16_t)length,
    };
    if (!packet.session ||
        (wire_kind == WIRE_AUDIO ? (!length || !packet.sequence) : length != 0) ||
        (wire_kind == WIRE_START ? packet.sequence != 0 : packet.sequence == 0)) return;
    if (!worker->received_talk || packet.session != worker->rx_session) {
        if (worker->received_talk && !newer_u16(packet.session, worker->rx_session)) return;
        worker->received_talk = true;
        worker->received_stop = false;
        worker->rx_session = packet.session;
        worker->rx_sequence = packet.sequence;
        worker->rx_seen = 1U;
    } else if (newer_u16(packet.sequence, worker->rx_sequence)) {
        if (worker->received_stop) return;
        uint16_t distance = (uint16_t)(packet.sequence - worker->rx_sequence);
        worker->rx_seen = distance < REPLAY_WINDOW ? (uint8_t)((worker->rx_seen << distance) | 1U) : 1U;
        worker->rx_sequence = packet.sequence;
    } else {
        /* Allow unique packets inside the jitter window, including audio
         * arriving after STOP whose sequence precedes that STOP boundary. */
        uint16_t distance = (uint16_t)(worker->rx_sequence - packet.sequence);
        if (wire_kind != WIRE_AUDIO || distance >= REPLAY_WINDOW ||
            (worker->rx_seen & (1U << distance))) return;
        worker->rx_seen |= 1U << distance;
    }
    if (wire_kind == WIRE_STOP) worker->received_stop = true;
    if (length) memcpy(packet.data, bytes + HEADER_SIZE, length);
    if (xQueueSend(s_voice_queue, &packet, 0) == pdTRUE) {
        if (wire_kind == WIRE_AUDIO) atomic_fetch_add(&s_metrics.rx_audio_queued, 1);
    } else if (wire_kind != WIRE_AUDIO) {
        aether_intercom_packet_t oldest;
        if (xQueueReceive(s_voice_queue, &oldest, 0) == pdTRUE &&
            oldest.kind == AETHER_INTERCOM_PACKET_AUDIO) atomic_fetch_add(&s_metrics.rx_voice_dropped, 1);
        xQueueSend(s_voice_queue, &packet, 0);
    } else atomic_fetch_add(&s_metrics.rx_voice_dropped, 1);
}

static void handle_packet(radio_worker_t *worker, const wire_packet_t *packet, int64_t now)
{
    const uint8_t *bytes = packet->bytes;
    if (bytes[3] <= WIRE_OFFER) {
        handle_discovery(worker, packet, now);
        return;
    }
    if (packet->length < HEADER_SIZE + TAG_SIZE) return;
    uint64_t target = get_u64(bytes + 4);
    if (!(bytes[3] == WIRE_PING && !target) && target != worker->local_epoch) return;
    if (!authenticated(worker, packet)) return;
    size_t length = packet->length - HEADER_SIZE - TAG_SIZE;
    if (bytes[3] >= WIRE_START) {
        if (bytes[3] == WIRE_AUDIO) worker->last_voice_at = now;
        handle_voice(worker, bytes, length);
        return;
    }
    if (length != 16 || get_u16(bytes + 12) || get_u16(bytes + 14)) return;
    uint64_t sender_epoch = get_u64(bytes + HEADER_SIZE);
    uint64_t challenge = get_u64(bytes + HEADER_SIZE + 8);
    if (!sender_epoch || !challenge) return;
    if (bytes[3] == WIRE_PING) {
        worker->pong_epoch = sender_epoch;
        worker->pong_challenge = challenge;
        return;
    }
    if (challenge != worker->ping_challenge) return;
    worker->ping_challenge = 0;
    if (worker->peer_epoch != sender_epoch) {
        worker->peer_epoch = sender_epoch;
        worker->received_talk = false;
        xQueueReset(s_voice_queue);
    }
    worker->last_pong = now;
    worker->last_rssi = packet->rssi;
    if (!worker->paired) {
        esp_err_t error = store_peer(&worker->peer);
        if (error != ESP_OK) ESP_LOGW(TAG, "pair save failed: %s", esp_err_to_name(error));
        worker->paired = true;
        worker->tentative_until = 0;
        close_search(worker);
        publish(worker, AETHER_INTERCOM_RADIO_PAIRED, error);
    }
    portENTER_CRITICAL(&s_lock);
    s_status.peer_online = true;
    s_status.rssi = packet->rssi;
    portEXIT_CRITICAL(&s_lock);
}

static bool take_voice(queued_voice_t *packet)
{
    portENTER_CRITICAL(&s_lock);
    bool available = s_tx_count != 0;
    if (available) {
        *packet = s_tx_queue[s_tx_head];
        s_tx_head = (s_tx_head + 1) % TX_CAPACITY;
        --s_tx_count;
    }
    portEXIT_CRITICAL(&s_lock);
    return available;
}

static void update_peer_rate(radio_worker_t *worker, int64_t now)
{
    if (!worker->paired || !worker->last_pong || worker->rate_unavailable) return;
    bool fast = now >= worker->fast_rate_hold &&
        worker->last_rssi >= (worker->fast_rate ? FAST_RATE_LEAVE_DBM : FAST_RATE_REENTER_DBM);
    if (fast == worker->fast_rate) return;
    esp_now_rate_config_t rate = {
        .phymode = fast ? WIFI_PHY_MODE_11G : WIFI_PHY_MODE_11B,
        .rate = fast ? WIFI_PHY_RATE_6M : WIFI_PHY_RATE_1M_L,
    };
    esp_err_t error = esp_now_set_peer_rate_config(worker->peer.mac, &rate);
    if (error == ESP_OK) worker->fast_rate = fast;
    else {
        /* Some driver builds do not expose per-peer rates. The default remains usable. */
        worker->rate_unavailable = true;
        ESP_LOGD(TAG, "peer rate configuration: %s", esp_err_to_name(error));
    }
}

static bool service_voice_tx(radio_worker_t *worker, int64_t now)
{
    if (!worker->paired || !worker->last_pong) return false;
    /* Preserve START and STOP. Let stale audio go instead of queuing an
     * entire old utterance behind an unrelated RF delay. */
    if (worker->have_pending_voice && voice_expired(&worker->pending_voice, now, TX_AUDIO_MAX_AGE_US)) {
        atomic_fetch_add(&s_metrics.tx_audio_expired, 1);
        worker->have_pending_voice = false;
    }
    while (!worker->have_pending_voice) {
        if (!take_voice(&worker->pending_voice)) return false;
        if (voice_expired(&worker->pending_voice, now, TX_AUDIO_MAX_AGE_US)) {
            atomic_fetch_add(&s_metrics.tx_audio_expired, 1);
            continue;
        }
        worker->have_pending_voice = true;
        worker->tx_retries = 0;
        worker->tx_retry_at = 0;
    }
    if (now < worker->tx_retry_at) return false;

    const aether_intercom_packet_t *packet = &worker->pending_voice.packet;
    if (packet->kind == AETHER_INTERCOM_PACKET_AUDIO) worker->last_voice_at = now;
    esp_err_t error = send_authenticated(worker, (wire_kind_t)(WIRE_START + packet->kind),
                                         worker->peer_epoch, packet->session, packet->sequence,
                                         packet->data, packet->length, now);
    if (error == ESP_OK) {
        worker->sending_voice = true;
        return true;
    }
    if (error == ESP_ERR_ESPNOW_NO_MEM) {
        atomic_fetch_add(&s_metrics.tx_submit_busy, 1);
        if (worker->tx_retries < 2 &&
            !voice_expired(&worker->pending_voice, now, TX_AUDIO_RETRY_AGE_US)) {
            ++worker->tx_retries;
            worker->tx_retry_at = now + TX_RETRY_PAUSE_US;
            return true;
        }
    }
    worker->have_pending_voice = false;
    return false;
}

static bool finish_tx(radio_worker_t *worker, int64_t now)
{
    if (!worker->tx_until) return true;
    tx_result_t result = (tx_result_t)atomic_load(&s_send_result);
    if (result == TX_WAITING && now < worker->tx_until) return false;
    worker->tx_until = 0;
    if (!worker->sending_voice) return true;
    worker->sending_voice = false;
    if (result == TX_ACKED) {
        if (worker->pending_voice.packet.kind == AETHER_INTERCOM_PACKET_AUDIO)
            atomic_fetch_add(&s_metrics.tx_audio_acked, 1);
    } else if (result == TX_FAILED) {
        atomic_fetch_add(&s_metrics.tx_mac_failed, 1);
        worker->fast_rate_hold = now + FAST_RATE_RETRY_US;
        if (worker->tx_retries == 0 &&
            !voice_expired(&worker->pending_voice, now, TX_AUDIO_RETRY_AGE_US)) {
            ++worker->tx_retries;
            worker->tx_retry_at = now + TX_RETRY_PAUSE_US;
            return false;
        }
    } else {
        atomic_fetch_add(&s_metrics.tx_callback_timeout, 1);
    }
    worker->have_pending_voice = false;
    return true;
}

static void service_tx(radio_worker_t *worker, int64_t now)
{
    if (!finish_tx(worker, now)) return;
    update_peer_rate(worker, now);
    /* Give a queued speech frame precedence over periodic heartbeats. A
     * delayed heartbeat gets its turn even if the link stays busy. */
    bool heartbeat_due = now >= worker->next_ping + 250000;
    if (!heartbeat_due && service_voice_tx(worker, now)) return;
    if (worker->pong_epoch) {
        uint8_t payload[16];
        put_u64(payload, worker->local_epoch);
        put_u64(payload + 8, worker->pong_challenge);
        send_authenticated(worker, WIRE_PONG, worker->pong_epoch, 0, 0, payload, sizeof(payload), now);
        worker->pong_epoch = 0;
        return;
    }
    if (worker->search_until && now >= worker->next_hello) {
        uint8_t hello[HELLO_SIZE];
        wire_prefix(hello, WIRE_HELLO);
        put_u64(hello + 4, worker->pair_nonce);
        memcpy(hello + 12, worker->public_key, PUBLIC_KEY_SIZE);
        worker->next_hello = now + 115000 + esp_random() % 35000;
        transmit(worker, broadcast, hello, sizeof(hello), now);
        return;
    }
    if (worker->search_until && now >= worker->next_offer && candidate_ready(worker, now) &&
        memcmp(worker->local_mac, worker->candidate.mac, MAC_SIZE) < 0) {
        worker->next_offer = now + 190000;
        esp_err_t error = worker->link_installed ? ESP_OK : install_candidate(worker, now);
        if (error == ESP_OK) {
            uint8_t offer[OFFER_SIZE];
            wire_prefix(offer, WIRE_OFFER);
            put_u64(offer + 4, worker->pair_nonce);
            put_u64(offer + 12, worker->candidate.nonce);
            memcpy(offer + 20, worker->candidate.mac, MAC_SIZE);
            transmit(worker, broadcast, offer, sizeof(offer), now);
        } else publish(worker, AETHER_INTERCOM_RADIO_SEARCHING, error);
        return;
    }
    if (worker->link_installed && now >= worker->next_ping) {
        uint64_t challenge = random_epoch();
        uint8_t payload[16];
        put_u64(payload, worker->local_epoch);
        put_u64(payload + 8, challenge);
        worker->next_ping = now + (worker->paired ? HEARTBEAT_US : 280000) + esp_random() % 40000;
        if (send_authenticated(worker, WIRE_PING, 0, 0, 0, payload, sizeof(payload), now) == ESP_OK) {
            worker->ping_challenge = challenge;
        }
        return;
    }
    if (heartbeat_due) (void)service_voice_tx(worker, now);
}

static void service_commands(radio_worker_t *worker, int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    bool search = s_search_requested, forget = s_forget_requested;
    s_search_requested = s_forget_requested = false;
    portEXIT_CRITICAL(&s_lock);
    if (forget && worker->paired) {
        esp_err_t error = store_peer(NULL);
        if (error == ESP_OK) {
            remove_peer(worker);
            xQueueReset(s_voice_queue);
            publish(worker, AETHER_INTERCOM_RADIO_READY, ESP_OK);
        } else publish(worker, AETHER_INTERCOM_RADIO_PAIRED, error);
    }
    if (search && !worker->paired && !worker->search_until) {
        esp_err_t error = open_search(worker, now);
        publish(worker, error == ESP_OK ? AETHER_INTERCOM_RADIO_SEARCHING :
                AETHER_INTERCOM_RADIO_READY, error);
    }
    if (worker->search_until && now >= worker->search_until) {
        remove_peer(worker);
        close_search(worker);
        publish(worker, AETHER_INTERCOM_RADIO_READY, ESP_ERR_TIMEOUT);
    }
    if (worker->tentative_until && now >= worker->tentative_until) {
        remove_peer(worker);
        worker->candidate = (candidate_t){0};
    }
    if (worker->last_pong && now - worker->last_pong >= PEER_OFFLINE_US) {
        worker->last_pong = 0;
        worker->have_pending_voice = false;
        worker->sending_voice = false;
        xQueueReset(s_voice_queue);
        portENTER_CRITICAL(&s_lock);
        s_status.peer_online = false;
        s_status.rssi = 0;
        reset_outgoing();
        portEXIT_CRITICAL(&s_lock);
    }
}

static void log_metrics(radio_worker_t *worker, int64_t now)
{
    /* Console output can block if USB serial is slow. Report accumulated
     * metrics once speech is idle so diagnostics cannot interrupt speech. */
    if (now < worker->next_metrics || (worker->last_voice_at && now - worker->last_voice_at < 180000)) return;
    worker->next_metrics = now + LINK_STATS_INTERVAL_US;
    aether_intercom_radio_metrics_t current;
    aether_intercom_radio_get_metrics(&current);
    const aether_intercom_radio_metrics_t *previous = &worker->previous_metrics;
    if (memcmp(&current, previous, sizeof(current))) {
        ESP_LOGI(TAG, "voice tx ack=%lu qdrop=%lu expired=%lu busy=%lu macfail=%lu tmo=%lu, "
                 "rx queued=%lu wire_drop=%lu voice_drop=%lu, rssi=%d rate=%s",
                 (unsigned long)(current.tx_audio_acked - previous->tx_audio_acked),
                 (unsigned long)(current.tx_queue_dropped - previous->tx_queue_dropped),
                 (unsigned long)(current.tx_audio_expired - previous->tx_audio_expired),
                 (unsigned long)(current.tx_submit_busy - previous->tx_submit_busy),
                 (unsigned long)(current.tx_mac_failed - previous->tx_mac_failed),
                 (unsigned long)(current.tx_callback_timeout - previous->tx_callback_timeout),
                 (unsigned long)(current.rx_audio_queued - previous->rx_audio_queued),
                 (unsigned long)(current.rx_wire_dropped - previous->rx_wire_dropped),
                 (unsigned long)(current.rx_voice_dropped - previous->rx_voice_dropped),
                 (int)worker->last_rssi, worker->fast_rate ? "6M" : "default/1M");
        worker->previous_metrics = current;
    }
}

static bool stop_requested(void)
{
    portENTER_CRITICAL(&s_lock);
    bool stop = s_stop_requested;
    portEXIT_CRITICAL(&s_lock);
    return stop;
}

static void worker_task(void *argument)
{
    (void)argument;
    radio_worker_t worker = {
        .private_key = MBEDTLS_SVC_KEY_ID_INIT,
        .auth_key = MBEDTLS_SVC_KEY_ID_INIT,
    };
    aether_wifi_handle_t wifi = aether_wifi_get_default();
    esp_err_t error = wifi ? aether_wifi_peer_acquire(wifi, 1500) : ESP_ERR_INVALID_STATE;
    bool lease = error == ESP_OK, initialized = false;
    if (error == ESP_OK) error = esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (error == ESP_OK) error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error == ESP_OK) error = esp_wifi_get_mac(WIFI_IF_STA, worker.local_mac);
    if (error == ESP_OK && psa_crypto_init() != PSA_SUCCESS) error = ESP_FAIL;
    if (error == ESP_OK) {
        worker.local_epoch = random_epoch();
        error = esp_now_init();
        initialized = error == ESP_OK;
    }
    if (error == ESP_OK) error = esp_now_register_recv_cb(recv_callback);
    if (error == ESP_OK) error = esp_now_register_send_cb(send_callback);
    if (error == ESP_OK) {
        esp_now_peer_info_t peer = {.channel = CHANNEL, .ifidx = WIFI_IF_STA};
        memcpy(peer.peer_addr, broadcast, MAC_SIZE);
        error = esp_now_add_peer(&peer);
    }
    if (error == ESP_OK) {
        esp_err_t saved = load_peer(&worker.peer);
        if (saved == ESP_OK && !memcmp(worker.peer.mac, worker.local_mac, MAC_SIZE)) {
            saved = ESP_ERR_INVALID_RESPONSE;
        }
        if (saved == ESP_OK) {
            saved = install_peer(&worker);
            worker.paired = saved == ESP_OK;
        }
        if (!worker.paired) wipe(&worker.peer, sizeof(worker.peer));
        publish(&worker, worker.paired ? AETHER_INTERCOM_RADIO_PAIRED : AETHER_INTERCOM_RADIO_READY,
                saved == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : saved);
        ESP_LOGI(TAG, "ready on channel %d, saved peer %s", CHANNEL, worker.paired ? "yes" : "no");
    }
    SemaphoreHandle_t wake = atomic_load(&s_worker_wake);
    while (error == ESP_OK && !stop_requested()) {
        wire_packet_t packet;
        /* Limit each drain so incoming traffic cannot starve speech sends
         * and stop/forget commands. The callback only posts and wakes us. */
        for (unsigned i = 0; i < 6 && xQueueReceive(s_wire_queue, &packet, 0) == pdTRUE; ++i) {
            handle_packet(&worker, &packet, esp_timer_get_time());
        }
        int64_t now = esp_timer_get_time();
        service_commands(&worker, now);
        service_tx(&worker, now);
        log_metrics(&worker, now);
        if (!uxQueueMessagesWaiting(s_wire_queue)) {
            /* Wake promptly on RX, a MAC completion, or newly queued audio.
             * Time-based work covers transient retry and discovery timers. */
            xSemaphoreTake(wake, pdMS_TO_TICKS(worker.have_pending_voice ? 2 : 12));
        }
    }
    close_search(&worker);
    remove_peer(&worker);
    if (initialized) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
    }
    xQueueReset(s_wire_queue);
    xQueueReset(s_voice_queue);
    if (lease) {
        esp_err_t released;
        do {
            released = aether_wifi_peer_release(wifi, 800);
        } while (released == ESP_ERR_TIMEOUT);
        if (released != ESP_OK && error == ESP_OK) error = released;
    }
    if (error != ESP_OK) ESP_LOGW(TAG, "station error: %s", esp_err_to_name(error));
    portENTER_CRITICAL(&s_lock);
    s_status = (aether_intercom_radio_status_t){
        .state = s_stop_requested || error == ESP_OK ? AETHER_INTERCOM_RADIO_STOPPED :
                 AETHER_INTERCOM_RADIO_ERROR,
        .last_error = error,
    };
    reset_outgoing();
    s_running = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

esp_err_t aether_intercom_radio_start(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_running) {
        bool stopping = s_stop_requested;
        portEXIT_CRITICAL(&s_lock);
        return stopping ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    s_running = true;
    s_stop_requested = s_search_requested = s_forget_requested = false;
    reset_outgoing();
    s_status = (aether_intercom_radio_status_t){.state = AETHER_INTERCOM_RADIO_STARTING};
    portEXIT_CRITICAL(&s_lock);
    if (!s_wire_queue) s_wire_queue = xQueueCreateStatic(RX_CAPACITY, sizeof(wire_packet_t),
                                                         s_wire_queue_data, &s_wire_queue_storage);
    if (!s_voice_queue) s_voice_queue = xQueueCreateStatic(RX_CAPACITY, sizeof(aether_intercom_packet_t),
                                                           s_voice_queue_data, &s_voice_queue_storage);
    if (!atomic_load(&s_worker_wake)) {
        atomic_store(&s_worker_wake, xSemaphoreCreateBinaryStatic(&s_worker_wake_storage));
    }
    reset_metrics();
    s_tx_session = (uint16_t)esp_random();
    if (s_wire_queue && s_voice_queue && atomic_load(&s_worker_wake) &&
        xTaskCreate(worker_task, "intercom_radio", 5632, NULL, 5, NULL) == pdPASS) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_lock);
    s_running = false;
    s_status.state = s_stop_requested ? AETHER_INTERCOM_RADIO_STOPPED : AETHER_INTERCOM_RADIO_ERROR;
    s_status.last_error = ESP_ERR_NO_MEM;
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t aether_intercom_radio_stop(uint32_t timeout_ms)
{
    portENTER_CRITICAL(&s_lock);
    s_stop_requested = true;
    reset_outgoing();
    if (!s_running) s_status = (aether_intercom_radio_status_t){.state = AETHER_INTERCOM_RADIO_STOPPED};
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
    if (!timeout_ms) return ESP_OK;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        bool running = s_running;
        portEXIT_CRITICAL(&s_lock);
        if (!running) return ESP_OK;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}

esp_err_t aether_intercom_radio_search(void)
{
    portENTER_CRITICAL(&s_lock);
    bool allowed = s_running && !s_stop_requested &&
                   (s_status.state == AETHER_INTERCOM_RADIO_READY ||
                    s_status.state == AETHER_INTERCOM_RADIO_SEARCHING);
    if (allowed && s_status.state == AETHER_INTERCOM_RADIO_READY) s_search_requested = true;
    portEXIT_CRITICAL(&s_lock);
    if (allowed) wake_worker();
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t aether_intercom_radio_forget(void)
{
    portENTER_CRITICAL(&s_lock);
    bool allowed = s_running && !s_stop_requested && s_status.state == AETHER_INTERCOM_RADIO_PAIRED;
    if (allowed) s_forget_requested = true;
    portEXIT_CRITICAL(&s_lock);
    if (allowed) wake_worker();
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void aether_intercom_radio_get_status(aether_intercom_radio_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void aether_intercom_radio_get_metrics(aether_intercom_radio_metrics_t *out)
{
    if (!out) return;
    *out = (aether_intercom_radio_metrics_t){
        .tx_audio_acked = atomic_load(&s_metrics.tx_audio_acked),
        .tx_queue_dropped = atomic_load(&s_metrics.tx_queue_dropped),
        .tx_audio_expired = atomic_load(&s_metrics.tx_audio_expired),
        .tx_submit_busy = atomic_load(&s_metrics.tx_submit_busy),
        .tx_mac_failed = atomic_load(&s_metrics.tx_mac_failed),
        .tx_callback_timeout = atomic_load(&s_metrics.tx_callback_timeout),
        .rx_audio_queued = atomic_load(&s_metrics.rx_audio_queued),
        .rx_wire_dropped = atomic_load(&s_metrics.rx_wire_dropped),
        .rx_voice_dropped = atomic_load(&s_metrics.rx_voice_dropped),
    };
}

esp_err_t aether_intercom_radio_send(aether_intercom_packet_kind_t kind,
                                  const void *data, size_t size)
{
    if ((unsigned)kind > AETHER_INTERCOM_PACKET_STOP || size > AETHER_INTERCOM_MAX_PAYLOAD ||
        (kind == AETHER_INTERCOM_PACKET_AUDIO ? !data || !size : size != 0)) return ESP_ERR_INVALID_ARG;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool open = s_tx_open;
    if (kind == AETHER_INTERCOM_PACKET_STOP) s_tx_open = false;
    if (!s_running || s_stop_requested || !s_status.peer_online ||
        (kind == AETHER_INTERCOM_PACKET_START ? open : !open)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (kind != AETHER_INTERCOM_PACKET_START && !++s_tx_sequence) ++s_tx_sequence;
    unsigned capacity = kind == AETHER_INTERCOM_PACKET_AUDIO ? TX_AUDIO_CAPACITY : TX_CAPACITY;
    if (s_tx_count >= capacity) remove_expired_queued_audio(now);
    if (s_tx_count >= capacity) {
        portEXIT_CRITICAL(&s_lock);
        if (kind == AETHER_INTERCOM_PACKET_AUDIO) atomic_fetch_add(&s_metrics.tx_queue_dropped, 1);
        return ESP_ERR_TIMEOUT;
    }
    if (kind == AETHER_INTERCOM_PACKET_START) {
        if (!++s_tx_session) ++s_tx_session;
        s_tx_sequence = 0;
        s_tx_open = true;
    }
    unsigned tail = (s_tx_head + s_tx_count) % TX_CAPACITY;
    queued_voice_t *queued = &s_tx_queue[tail];
    queued->queued_us = now;
    aether_intercom_packet_t *packet = &queued->packet;
    packet->kind = kind;
    packet->session = s_tx_session;
    packet->sequence = s_tx_sequence;
    packet->length = (uint16_t)size;
    if (size) memcpy(packet->data, data, size);
    ++s_tx_count;
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
    return ESP_OK;
}

bool aether_intercom_radio_receive(aether_intercom_packet_t *out, uint32_t timeout_ms)
{
    if (!out || !s_voice_queue) return false;
    return xQueueReceive(s_voice_queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
