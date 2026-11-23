#include "app_intercom.h"

#include "aether_board_pins.h"
#include "aether_intercom_audio.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define FRAME_US (AETHER_INTERCOM_AUDIO_FRAME_MS * 1000)
#define RX_SLOTS 16
#define RX_PREBUFFER 5
#define RX_START_TIMEOUT_US 150000
#define RX_REBUFFER_US 100000
#define RX_REBUFFER_GAPS 3
#define RX_IDLE_US 440000
#define RX_DRAIN_US 40000
#define KEY_RELEASE_US 12000

_Static_assert(AETHER_INTERCOM_AUDIO_FRAME_BYTES <= sizeof(((aether_intercom_packet_t *)0)->data),
               "Speech frame exceeds the ESP-NOW payload");

typedef enum { VOICE_IDLE, VOICE_TX, VOICE_RX } voice_mode_t;
typedef enum { COMMAND_NONE, COMMAND_SEARCH, COMMAND_FORGET } intercom_command_t;

typedef struct {
    uint16_t sequence;
    bool present;
    uint8_t data[AETHER_INTERCOM_AUDIO_FRAME_BYTES];
} voice_slot_t;

typedef struct {
    voice_mode_t mode;
    bool codec_pending;
    bool playing;
    bool stop_received;
    bool rebuffering;
    bool screen_talk;
    unsigned buffered;
    unsigned empty_frames;
    unsigned capture_errors;
    uint16_t session;
    uint16_t finished_session;
    uint16_t next_sequence;
    uint16_t stop_sequence;
    int64_t talk_started_us;
    int64_t last_packet_us;
    int64_t first_audio_us;
    int64_t stop_arrived_us;
    int64_t rebuffer_until_us;
    int64_t next_play_us;
    int64_t last_write_us;
    int64_t release_us;
    struct {
        unsigned captured, sent, capture_errors, received, played, concealed, rebuffers, late;
    } metrics;
    voice_slot_t slots[RX_SLOTS];
    uint8_t capture[AETHER_INTERCOM_AUDIO_FRAME_BYTES];
} intercom_voice_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "intercom";
static struct {
    ac101s_audio_handle_t codec;
    app_io_handle_t io;
    uint32_t generation, session_generation;
    bool requested, worker_running, session_running, key_armed, key_pending;
    bool screen_talk_held, screen_talk_pending;
    int64_t entered_at_us;
    voice_mode_t mode;
    intercom_command_t command;
    esp_err_t error;
} s;

static void publish(voice_mode_t mode, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s.mode = mode;
    s.error = error;
    portEXIT_CRITICAL(&s_lock);
}

static bool still_requested(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    const bool current = s.requested && s.generation == generation;
    portEXIT_CRITICAL(&s_lock);
    return current;
}

static bool screen_talk_held(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    const bool held = s.requested && s.generation == generation && s.screen_talk_held;
    portEXIT_CRITICAL(&s_lock);
    return held;
}

static void finish_voice(intercom_voice_t *voice)
{
    if (voice->mode == VOICE_TX) {
        (void)aether_intercom_radio_send(AETHER_INTERCOM_PACKET_STOP, NULL, 0);
        if (voice->metrics.captured || voice->metrics.capture_errors) {
            ESP_LOGI(TAG, "TX %lld ms captured=%u queued=%u read_errors=%u",
                     (long long)((esp_timer_get_time() - voice->talk_started_us) / 1000),
                     voice->metrics.captured, voice->metrics.sent, voice->metrics.capture_errors);
        }
    } else if (voice->mode == VOICE_RX) {
        voice->finished_session = voice->session;
        if (voice->metrics.received) {
            ESP_LOGI(TAG, "RX session=%u received=%u played=%u conceal=%u rebuffer=%u late=%u",
                     voice->session, voice->metrics.received, voice->metrics.played,
                     voice->metrics.concealed, voice->metrics.rebuffers, voice->metrics.late);
        }
    }
    voice->mode = VOICE_IDLE;
    voice->playing = false;
    voice->rebuffering = false;
    voice->buffered = 0;
    voice->empty_frames = 0;
    memset(&voice->metrics, 0, sizeof(voice->metrics));
    memset(voice->slots, 0, sizeof(voice->slots));
}

static esp_err_t close_codec(intercom_voice_t *voice, aether_intercom_audio_handle_t audio)
{
    if (!voice->codec_pending) return ESP_OK;
    const esp_err_t error = aether_intercom_audio_stop(audio);
    if (error == ESP_OK) voice->codec_pending = false;
    return error;
}

static bool initial_frame_fits(const intercom_voice_t *voice, uint16_t sequence)
{
    if (voice->playing || (uint16_t)(voice->next_sequence - sequence) >= RX_SLOTS) return false;
    for (unsigned i = 0; i < RX_SLOTS; ++i) {
        const voice_slot_t *slot = &voice->slots[i];
        if (slot->present && (uint16_t)(slot->sequence - sequence) >= RX_SLOTS) return false;
    }
    return true;
}

static void receive_voice(intercom_voice_t *voice, const aether_intercom_packet_t *packet,
                          bool local_has_priority, int64_t now)
{
    const bool audio = packet->kind == AETHER_INTERCOM_PACKET_AUDIO;
    const bool start = packet->kind == AETHER_INTERCOM_PACKET_START;
    if (!packet->session || packet->session == voice->finished_session) return;
    if (audio && packet->length != AETHER_INTERCOM_AUDIO_FRAME_BYTES) return;
    if (!audio && !start) {
        if (packet->kind == AETHER_INTERCOM_PACKET_STOP && voice->mode == VOICE_RX &&
            packet->session == voice->session) {
            const uint16_t distance = (uint16_t)(packet->sequence - voice->next_sequence);
            if (distance < RX_SLOTS) {
                voice->stop_received = true;
                voice->stop_sequence = packet->sequence;
                voice->stop_arrived_us = now;
            }
        }
        return;
    }
    if (voice->mode == VOICE_TX) {
        if (local_has_priority) {
            voice->finished_session = packet->session;
            return;
        }
        finish_voice(voice);
    }
    if (voice->mode == VOICE_RX && voice->session != packet->session) {
        /* The first frame of a new burst is authoritative. Late packets from
         * the previous burst are discarded by the completed-session guard. */
        finish_voice(voice);
    }
    if (voice->mode == VOICE_IDLE) {
        voice->mode = VOICE_RX;
        voice->session = packet->session;
        voice->next_sequence = start ? 1 : packet->sequence;
        voice->stop_received = false;
        voice->next_play_us = 0;
        voice->last_packet_us = now;
        voice->first_audio_us = 0;
        voice->last_write_us = 0;
        publish(VOICE_RX, ESP_OK);
    }
    if (!audio) return;
    const uint16_t distance = (uint16_t)(packet->sequence - voice->next_sequence);
    if (distance >= RX_SLOTS) {
        const bool rewind = distance >= 0x8000U && initial_frame_fits(voice, packet->sequence);
        const bool resync = distance < 0x8000U && !voice->buffered &&
                            now - voice->last_packet_us >= 4 * FRAME_US;
        if (!rewind && !resync) {
            ++voice->metrics.late;
            return;
        }
        voice->next_sequence = packet->sequence;
        if (resync) voice->stop_received = false;
    }
    voice_slot_t *slot = &voice->slots[packet->sequence % RX_SLOTS];
    if (slot->present) return;
    slot->sequence = packet->sequence;
    slot->present = true;
    memcpy(slot->data, packet->data, sizeof(slot->data));
    ++voice->buffered;
    ++voice->metrics.received;
    if (voice->buffered == 1) {
        if (!voice->playing) voice->first_audio_us = now;
        else if (voice->rebuffering && !voice->rebuffer_until_us) {
            voice->rebuffer_until_us = now + RX_REBUFFER_US;
        }
    }
    voice->last_packet_us = now;
}

static uint16_t next_voice_sequence(uint16_t sequence)
{
    return sequence == UINT16_MAX ? 1 : (uint16_t)(sequence + 1);
}

static voice_slot_t *next_voice_slot(intercom_voice_t *voice)
{
    voice_slot_t *slot = &voice->slots[voice->next_sequence % RX_SLOTS];
    return slot->present && slot->sequence == voice->next_sequence ? slot : NULL;
}

static void resume_buffered_voice(intercom_voice_t *voice)
{
    uint16_t sequence = voice->next_sequence;
    for (unsigned i = 0; i < RX_SLOTS; ++i, sequence = next_voice_sequence(sequence)) {
        voice_slot_t *slot = &voice->slots[sequence % RX_SLOTS];
        if (slot->present && slot->sequence == sequence) {
            voice->next_sequence = sequence;
            break;
        }
    }
    voice->rebuffering = false;
    voice->empty_frames = 0;
}

static void playback_step(intercom_voice_t *voice, aether_intercom_audio_handle_t audio, int64_t now)
{
    if ((voice->stop_received && voice->next_sequence == voice->stop_sequence) ||
        now - voice->last_packet_us >= RX_IDLE_US) {
        if (voice->playing && now - voice->last_write_us < RX_DRAIN_US) return;
        finish_voice(voice);
        publish(VOICE_IDLE, ESP_OK);
        return;
    }
    if (!voice->playing) {
        if (!voice->buffered) return;
        const bool brief_talk_ready = voice->stop_received && now - voice->stop_arrived_us >= FRAME_US;
        if (voice->buffered < RX_PREBUFFER && !brief_talk_ready &&
            now - voice->first_audio_us < RX_START_TIMEOUT_US) return;
        voice->codec_pending = true;
        const esp_err_t error = aether_intercom_audio_start(audio, AETHER_INTERCOM_AUDIO_PLAYBACK);
        if (error != ESP_OK) {
            finish_voice(voice);
            publish(VOICE_IDLE, error);
            return;
        }
        voice->playing = true;
        /* Submit two frames at startup. The second can wait briefly for an
         * I2S descriptor, leaving PCM ahead of the hardware playback cursor. */
        voice->next_play_us = esp_timer_get_time() - FRAME_US;
    }

    if (now < voice->next_play_us) return;
    if (voice->rebuffering && (voice->buffered >= RX_PREBUFFER || voice->stop_received ||
        (voice->buffered && voice->rebuffer_until_us && now >= voice->rebuffer_until_us))) {
        resume_buffered_voice(voice);
    }
    voice_slot_t *slot = voice->rebuffering ? NULL : next_voice_slot(voice);
    esp_err_t error = ESP_ERR_INVALID_ARG;
    if (slot) {
        error = aether_intercom_audio_play(audio, slot->data);
        slot->present = false;
        --voice->buffered;
        voice->next_sequence = next_voice_sequence(voice->next_sequence);
        voice->empty_frames = 0;
        if (error == ESP_OK) ++voice->metrics.played;
    } else if (!voice->rebuffering) {
        if (voice->buffered || voice->stop_received) {
            voice->next_sequence = next_voice_sequence(voice->next_sequence);
            voice->empty_frames = 0;
        } else if (++voice->empty_frames >= RX_REBUFFER_GAPS) {
            voice->rebuffering = true;
            voice->rebuffer_until_us = 0;
            ++voice->metrics.rebuffers;
        }
    }
    if (error == ESP_ERR_INVALID_ARG) {
        error = aether_intercom_audio_conceal(audio);
        if (error == ESP_OK) ++voice->metrics.concealed;
    }
    const int64_t completed = esp_timer_get_time();
    voice->next_play_us += FRAME_US;
    if (voice->next_play_us + 2 * FRAME_US < completed) voice->next_play_us = completed;
    if (error != ESP_OK) {
        finish_voice(voice);
        publish(VOICE_IDLE, error);
    } else {
        voice->last_write_us = completed;
    }
}

static bool talk_held(uint32_t generation, bool screen_talk)
{
    return screen_talk ? screen_talk_held(generation) : gpio_get_level(AETHER_GPIO_BOOT_KEY) == 0;
}

static void capture_step(intercom_voice_t *voice, aether_intercom_audio_handle_t audio,
                         uint32_t generation, int64_t now)
{
    if (!talk_held(generation, voice->screen_talk)) {
        if (!voice->release_us) voice->release_us = now;
        if (voice->screen_talk || now - voice->release_us >= KEY_RELEASE_US) {
            finish_voice(voice);
            publish(VOICE_IDLE, ESP_OK);
        }
        return;
    }
    voice->release_us = 0;
    /* The ADC produces exactly 20 ms of new PCM per frame. Wait for its DMA
     * descriptors directly so scheduler lateness cannot slow the stream. */
    const esp_err_t error = aether_intercom_audio_capture(audio, voice->capture);
    if (error == ESP_OK) {
        ++voice->metrics.captured;
        voice->capture_errors = 0;
        if (still_requested(generation) && aether_intercom_radio_send(AETHER_INTERCOM_PACKET_AUDIO,
                    voice->capture, sizeof(voice->capture)) == ESP_OK) {
            ++voice->metrics.sent;
        }
    } else {
        ++voice->metrics.capture_errors;
        if (++voice->capture_errors >= 3) {
            finish_voice(voice);
            publish(VOICE_IDLE, error);
        }
    }
}

static void begin_capture(intercom_voice_t *voice, aether_intercom_audio_handle_t audio,
                          uint32_t generation, bool screen_talk)
{
    voice->codec_pending = true;
    esp_err_t error = aether_intercom_audio_start(audio, AETHER_INTERCOM_AUDIO_CAPTURE);
    if (error == ESP_OK && still_requested(generation) && talk_held(generation, screen_talk)) {
        error = aether_intercom_radio_send(AETHER_INTERCOM_PACKET_START, NULL, 0);
        if (error == ESP_OK) {
            voice->mode = VOICE_TX;
            voice->screen_talk = screen_talk;
            voice->release_us = 0;
            voice->talk_started_us = esp_timer_get_time();
            voice->capture_errors = 0;
        }
    }
    publish(voice->mode, error);
}

static void run_session(uint32_t generation, ac101s_audio_handle_t codec, app_io_handle_t io)
{
    aether_intercom_audio_handle_t audio = NULL;
    intercom_voice_t *voice = NULL;
    bool radio_started = false;
    bool ready_for_key = false;
    bool first_search = true;
    bool media_pending = true;
    esp_err_t media_error = ESP_OK;
    while (still_requested(generation) && media_pending) {
        const esp_err_t error = app_io_media_release_status(io, &media_pending, &media_error);
        if (error != ESP_OK) {
            publish(VOICE_IDLE, error);
            return;
        }
        if (media_pending) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (still_requested(generation)) {
        voice = heap_caps_calloc(1, sizeof(*voice), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        esp_err_t error = voice ? aether_intercom_audio_new(codec, &audio) : ESP_ERR_NO_MEM;
        if (error == ESP_OK) {
            error = aether_intercom_radio_start();
            radio_started = error == ESP_OK;
        }
        publish(VOICE_IDLE, error);
    }
    while (radio_started && still_requested(generation)) {
        aether_intercom_radio_status_t radio;
        aether_intercom_radio_get_status(&radio);
        const bool online = radio.state == AETHER_INTERCOM_RADIO_PAIRED && radio.peer_online;

        portENTER_CRITICAL(&s_lock);
        const bool current = s.requested && s.generation == generation;
        intercom_command_t command = current ? s.command : COMMAND_NONE;
        const bool pressed = current && s.key_pending;
        const bool screen_pressed = current && s.screen_talk_pending;
        if (current) {
            s.command = COMMAND_NONE;
            s.key_pending = false;
            s.screen_talk_pending = false;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!current) break;
        if (first_search && radio.state == AETHER_INTERCOM_RADIO_READY) {
            first_search = false;
            if (command == COMMAND_NONE) command = COMMAND_SEARCH;
        } else if (radio.state == AETHER_INTERCOM_RADIO_PAIRED) {
            first_search = false;
        }

        if (command != COMMAND_NONE || (!online && voice->mode != VOICE_IDLE)) {
            finish_voice(voice);
            publish(VOICE_IDLE, ESP_OK);
        }
        if (command != COMMAND_NONE) {
            const esp_err_t error = command == COMMAND_SEARCH ? aether_intercom_radio_search() :
                                                                aether_intercom_radio_forget();
            voice->finished_session = 0;
            publish(VOICE_IDLE, error);
        }
        if (voice->codec_pending && !voice->playing && voice->mode != VOICE_TX) {
            const esp_err_t error = close_codec(voice, audio);
            if (error != ESP_OK) {
                publish(VOICE_IDLE, error);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }
        if (gpio_get_level(AETHER_GPIO_BOOT_KEY) != 0) ready_for_key = true;

        aether_intercom_packet_t packet;
        const bool idle = voice->mode == VOICE_IDLE;
        for (unsigned i = 0; i < RX_SLOTS && aether_intercom_radio_receive(&packet,
             idle && online && !pressed && !screen_pressed && i == 0 ? 10 : 0); ++i) {
            if (online && command == COMMAND_NONE)
                receive_voice(voice, &packet, radio.local_has_priority, esp_timer_get_time());
        }
        if (!still_requested(generation)) break;
        /* A direction change closes the ADC before initializing the DAC. */
        if (voice->mode == VOICE_RX && !voice->playing && voice->codec_pending) {
            const esp_err_t error = close_codec(voice, audio);
            if (error != ESP_OK) {
                publish(VOICE_IDLE, error);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }
        if (online && command == COMMAND_NONE && voice->mode == VOICE_IDLE) {
            if (screen_pressed && talk_held(generation, true)) begin_capture(voice, audio, generation, true);
            else if (pressed && ready_for_key && talk_held(generation, false))
                begin_capture(voice, audio, generation, false);
        }
        const int64_t now = esp_timer_get_time();
        if (voice->mode == VOICE_TX) {
            capture_step(voice, audio, generation, now);
            if (voice->mode == VOICE_TX && !voice->release_us && !voice->capture_errors) continue;
            vTaskDelay(1);
        } else if (voice->mode == VOICE_RX) {
            playback_step(voice, audio, now);
            if (voice->mode == VOICE_RX && voice->playing) {
                const int64_t time = esp_timer_get_time();
                const bool draining = (voice->stop_received && voice->next_sequence == voice->stop_sequence) ||
                    time - voice->last_packet_us >= RX_IDLE_US;
                const int64_t deadline = draining ? voice->last_write_us + RX_DRAIN_US : voice->next_play_us;
                const int64_t wait_us = deadline - time;
                if (wait_us <= 0) continue;
                const uint32_t wait_ms = wait_us > 4000 ? 4 : (uint32_t)wait_us / 1000;
                vTaskDelay(wait_ms ? pdMS_TO_TICKS(wait_ms) : 1);
            } else vTaskDelay(pdMS_TO_TICKS(4));
        } else {
            vTaskDelay(pdMS_TO_TICKS(online ? 4 : 20));
        }
    }
    if (voice) finish_voice(voice);
    if (radio_started) (void)aether_intercom_radio_stop(0);
    while (voice && voice->codec_pending) {
        const esp_err_t error = close_codec(voice, audio);
        if (error == ESP_OK) break;
        publish(VOICE_IDLE, error);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (audio) aether_intercom_audio_del(audio);
    heap_caps_free(voice);
    while (radio_started && aether_intercom_radio_stop(100) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void intercom_task(void *unused)
{
    (void)unused;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const bool requested = s.requested;
        const uint32_t generation = s.generation;
        ac101s_audio_handle_t codec = s.codec;
        app_io_handle_t io = s.io;
        s.session_running = requested;
        s.session_generation = generation;
        if (!requested) s.worker_running = false;
        portEXIT_CRITICAL(&s_lock);
        if (!requested) break;

        run_session(generation, codec, io);
        /* If initialization failed, keep the error visible until retry/exit. */
        while (still_requested(generation)) vTaskDelay(pdMS_TO_TICKS(50));
        publish(VOICE_IDLE, ESP_OK);
    }
    vTaskDelete(NULL);
}

esp_err_t app_intercom_enter(ac101s_audio_handle_t codec, app_io_handle_t io)
{
    if (!codec || !io) return ESP_ERR_INVALID_ARG;
    const esp_err_t release = app_io_release_media(io);
    if (release != ESP_OK) return release;

    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s.codec = codec;
    s.io = io;
    s.requested = true;
    s.key_pending = false;
    s.screen_talk_held = s.screen_talk_pending = false;
    s.key_armed = true;
    s.entered_at_us = now;
    s.command = COMMAND_NONE;
    s.error = ESP_OK;
    ++s.generation;
    const bool launch = !s.worker_running;
    s.worker_running = true;
    portEXIT_CRITICAL(&s_lock);
    if (launch && xTaskCreate(intercom_task, "intercom", 6144, NULL, 5, NULL) != pdPASS) {
        app_intercom_leave();
        portENTER_CRITICAL(&s_lock);
        s.worker_running = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "could not create audio task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_intercom_leave(void)
{
    portENTER_CRITICAL(&s_lock);
    s.requested = false;
    s.key_armed = false;
    s.key_pending = false;
    s.screen_talk_held = s.screen_talk_pending = false;
    s.command = COMMAND_NONE;
    portEXIT_CRITICAL(&s_lock);
}

bool app_intercom_busy(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool busy = s.worker_running;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

bool app_intercom_closing(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool closing = s.session_running && (!s.requested || s.generation != s.session_generation);
    portEXIT_CRITICAL(&s_lock);
    return closing;
}

bool app_intercom_user_key(int64_t pressed_at_us)
{
    portENTER_CRITICAL(&s_lock);
    const bool owned = s.key_armed;
    if (owned && pressed_at_us >= s.entered_at_us) s.key_pending = true;
    portEXIT_CRITICAL(&s_lock);
    return owned;
}

void app_intercom_set_screen_talk(bool held)
{
    portENTER_CRITICAL(&s_lock);
    if (!held) s.screen_talk_held = s.screen_talk_pending = false;
    else if (s.requested && s.mode == VOICE_IDLE && !s.screen_talk_held)
        s.screen_talk_held = s.screen_talk_pending = true;
    portEXIT_CRITICAL(&s_lock);
}

void app_intercom_get_status(app_intercom_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = (app_intercom_status_t){
        .active = s.requested,
        .transmitting = s.mode == VOICE_TX,
        .receiving = s.mode == VOICE_RX,
        .audio_error = s.error,
    };
    portEXIT_CRITICAL(&s_lock);
    aether_intercom_radio_get_status(&status->radio);
}

static esp_err_t post_command(intercom_command_t command)
{
    portENTER_CRITICAL(&s_lock);
    const bool accepted = s.requested && s.mode == VOICE_IDLE && s.command == COMMAND_NONE;
    if (accepted) s.command = command;
    portEXIT_CRITICAL(&s_lock);
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t app_intercom_search(void) { return post_command(COMMAND_SEARCH); }
esp_err_t app_intercom_forget(void) { return post_command(COMMAND_FORGET); }
