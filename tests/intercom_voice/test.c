#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aether_intercom_audio.h"
#include "aether_intercom_radio.h"

#include "voice-types.inc"

static unsigned starts, stops, radio_stops, radio_audio, captured, written, holes, silent;
static unsigned capture_starts, radio_starts;
static unsigned write_tags[32];
static int64_t write_started[32];
static int64_t time_us, last_write_finished;
static bool key_released, screen_held, release_on_capture_start;

#define ESP_LOGI(...) ((void)0)
#define AETHER_GPIO_BOOT_KEY 1

static void publish(voice_mode_t mode, esp_err_t error)
{
    (void)mode;
    assert(error == ESP_OK);
}

esp_err_t aether_intercom_radio_send(aether_intercom_packet_kind_t kind,
                                    const void *data, size_t size)
{
    if (kind == AETHER_INTERCOM_PACKET_AUDIO) {
        assert(data && size == AETHER_INTERCOM_AUDIO_FRAME_BYTES);
        ++radio_audio;
        return ESP_OK;
    }
    if (kind == AETHER_INTERCOM_PACKET_START) {
        assert(!data && !size);
        ++radio_starts;
        return ESP_OK;
    }
    assert(kind == AETHER_INTERCOM_PACKET_STOP && !data && !size);
    ++radio_stops;
    return ESP_OK;
}

esp_err_t aether_intercom_audio_capture(aether_intercom_audio_handle_t audio,
                                       uint8_t data[AETHER_INTERCOM_AUDIO_FRAME_BYTES])
{
    assert(audio && data);
    ++captured;
    return ESP_OK;
}

esp_err_t aether_intercom_audio_start(aether_intercom_audio_handle_t audio,
                                      aether_intercom_audio_mode_t mode)
{
    assert(audio);
    if (mode == AETHER_INTERCOM_AUDIO_CAPTURE) {
        ++capture_starts;
        if (release_on_capture_start) screen_held = false;
    } else {
        assert(mode == AETHER_INTERCOM_AUDIO_PLAYBACK);
        ++starts;
    }
    return ESP_OK;
}

esp_err_t aether_intercom_audio_stop(aether_intercom_audio_handle_t audio)
{
    assert(audio);
    ++stops;
    return ESP_OK;
}

esp_err_t aether_intercom_audio_play(aether_intercom_audio_handle_t audio,
                                     const uint8_t data[AETHER_INTERCOM_AUDIO_FRAME_BYTES])
{
    assert(audio && written < sizeof(write_tags) / sizeof(write_tags[0]));
    write_started[written] = time_us;
    write_tags[written++] = data[0];
    time_us += 7000;
    last_write_finished = time_us;
    return ESP_OK;
}

esp_err_t aether_intercom_audio_silence(aether_intercom_audio_handle_t audio)
{
    assert(audio);
    ++silent;
    return ESP_OK;
}

esp_err_t aether_intercom_audio_conceal(aether_intercom_audio_handle_t audio)
{
    assert(audio);
    ++holes;
    return ESP_OK;
}

static int64_t esp_timer_get_time(void) { return time_us; }
static bool still_requested(uint32_t generation) { return generation == 1; }
static bool screen_talk_held(uint32_t generation) { return still_requested(generation) && screen_held; }
static int gpio_get_level(int pin) { assert(pin == AETHER_GPIO_BOOT_KEY); return key_released; }

#include "voice-production.inc"

static void send(intercom_voice_t *voice, aether_intercom_packet_kind_t kind,
                 uint16_t session, uint16_t seq, bool local_priority, int64_t stamp)
{
    aether_intercom_packet_t packet = {.kind = kind, .session = session, .sequence = seq};
    if (kind == AETHER_INTERCOM_PACKET_AUDIO) {
        packet.length = AETHER_INTERCOM_AUDIO_FRAME_BYTES;
        packet.data[0] = (uint8_t)seq;
    }
    receive_voice(voice, &packet, local_priority, stamp);
}

static void reset_audio(void)
{
    starts = stops = radio_stops = radio_audio = captured = written = holes = silent = 0;
    radio_starts = capture_starts = 0;
    screen_held = release_on_capture_start = false;
    time_us = last_write_finished = 0;
    key_released = false;
}

static void play_at(intercom_voice_t *voice, int64_t now)
{
    assert(now >= time_us);
    time_us = now;
    playback_step(voice, (void *)1, now);
}

static void play_until(intercom_voice_t *voice, int64_t until)
{
    while (time_us < until) {
        const int64_t before = time_us;
        playback_step(voice, (void *)1, before);
        if (time_us == before) time_us += 1000;
    }
}

static void test_arbitration_and_short_burst(void)
{
    reset_audio();
    intercom_voice_t winner = {.mode=VOICE_TX};
    intercom_voice_t yielding = {.mode=VOICE_TX};
    send(&winner, AETHER_INTERCOM_PACKET_START, 11, 0, true, 1000);
    send(&yielding, AETHER_INTERCOM_PACKET_START, 22, 0, false, 1000);
    assert(winner.mode == VOICE_TX && winner.finished_session == 11);
    assert(yielding.mode == VOICE_RX && yielding.session == 22 && radio_stops == 1);
    send(&winner, AETHER_INTERCOM_PACKET_AUDIO, 11, 1, true, 1000);
    assert(winner.mode == VOICE_TX && radio_stops == 1);
    puts("PASS simultaneous press yields a single talker; losing frames ignored by winner");

    send(&yielding, AETHER_INTERCOM_PACKET_AUDIO, 22, 3, false, 1500);
    send(&yielding, AETHER_INTERCOM_PACKET_STOP, 22, 4, false, 2000);
    send(&yielding, AETHER_INTERCOM_PACKET_AUDIO, 22, 1, false, 2400);
    send(&yielding, AETHER_INTERCOM_PACKET_AUDIO, 22, 1, false, 3200);
    send(&yielding, AETHER_INTERCOM_PACKET_AUDIO, 22, RX_SLOTS + 1, false, 3500);
    assert(yielding.buffered == 2 && yielding.stop_received);
    play_at(&yielding, 4000);
    assert(!starts && !written);
    while (written < 2 && time_us < 150000) play_until(&yielding, time_us + 1000);
    assert(starts == 1 && written == 2 && holes == 1 && write_tags[0] == 1 && write_tags[1] == 3);
    assert(!yielding.buffered && yielding.next_sequence == 4);
    puts("PASS short peer STOP allows reordered tail, rejects duplicates/out-of-window frames and conceals a missing frame");

    play_at(&yielding, last_write_finished + RX_DRAIN_US - 1);
    assert(yielding.mode == VOICE_RX && yielding.playing && !stops);
    play_at(&yielding, last_write_finished + RX_DRAIN_US);
    assert(yielding.mode == VOICE_IDLE && !yielding.playing && yielding.finished_session == 22);
    assert(close_codec(&yielding, (void *)1) == ESP_OK && stops == 1);
    send(&yielding, AETHER_INTERCOM_PACKET_AUDIO, 22, 3, false, time_us + 1000);
    assert(yielding.mode == VOICE_IDLE);
    puts("PASS peer STOP retains audio for 40 ms after last PCM write completes and rejects old audio");
}

static void test_forward_rebase(void)
{
    reset_audio();
    intercom_voice_t voice = {0};
    send(&voice, AETHER_INTERCOM_PACKET_START, 23, 0, false, 160000);
    send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 23, 60, false, 241000);
    assert(voice.buffered == 1 && voice.next_sequence == 60);
    send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 23, 1, false, 242000);
    assert(voice.buffered == 1 && voice.next_sequence == 60);
    puts("PASS sustained transport loss rebases jitter window and rejects stale audio");
}

static void test_prebuffer_and_rebuffer(void)
{
    reset_audio();
    intercom_voice_t voice = {0};
    send(&voice, AETHER_INTERCOM_PACKET_START, 24, 0, false, 1000);
    for (unsigned i = 1; i <= RX_PREBUFFER; ++i) {
        const int64_t arrival = (int64_t)i * FRAME_US;
        play_until(&voice, arrival);
        assert(!starts && !written);
        send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 24, (uint16_t)i, false, arrival);
    }
    play_until(&voice, time_us + FRAME_US);
    assert(starts == 1 && written == 2 && write_tags[0] == 1 && write_tags[1] == 2);
    assert(write_started[1] - write_started[0] < FRAME_US);
    puts("PASS waits for real startup frames and fills two DMA blocks without a frame-period gap");

    play_until(&voice, 250000);
    assert(written == RX_PREBUFFER && holes >= RX_REBUFFER_GAPS);
    assert(voice.rebuffering && voice.metrics.rebuffers == 1);
    const unsigned before = written;
    const uint16_t resumed_sequence = RX_PREBUFFER + 4;
    const int64_t first = time_us + 1000;
    for (unsigned i = 0; i < RX_PREBUFFER; ++i) {
        const int64_t arrival = first + (int64_t)i * FRAME_US;
        play_until(&voice, arrival);
        assert(written == before);
        send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 24,
             (uint16_t)(resumed_sequence + i), false, arrival);
    }
    play_until(&voice, time_us + FRAME_US);
    assert(written >= before + 1 && write_tags[before] == resumed_sequence);
    assert(!voice.rebuffering && voice.metrics.rebuffers == 1);
    puts("PASS sustained empty-buffer gap limits concealment and rebuilds the buffer before playing resumed speech");
}

static void test_first_audio_anchors_startup_limit(void)
{
    reset_audio();
    intercom_voice_t voice = {0};
    send(&voice, AETHER_INTERCOM_PACKET_START, 25, 0, false, 1000);
    play_until(&voice, 96000);
    assert(!starts && !written);
    send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 25, 1, false, time_us);
    const int64_t audio_at = time_us;
    play_until(&voice, audio_at + RX_START_TIMEOUT_US - 1000);
    assert(!starts && !written);
    play_until(&voice, audio_at + RX_START_TIMEOUT_US + 3000);
    assert(starts == 1 && written == 1 && write_tags[0] == 1);
    puts("PASS bounded fallback for short speech with a lost STOP starts from the first audio");
}

static void test_receive_rewinds_before_playing(void)
{
    reset_audio();
    intercom_voice_t voice = {0};
    /* START is lost and frames 12, 10, 11 happen to arrive in this order. */
    send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 26, 12, false, 1000);
    send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 26, 10, false, 2000);
    send(&voice, AETHER_INTERCOM_PACKET_AUDIO, 26, 11, false, 3000);
    assert(voice.next_sequence == 10 && voice.buffered == 3);
    send(&voice, AETHER_INTERCOM_PACKET_STOP, 26, 13, false, 4000);
    play_at(&voice, 4000);
    while (written < 3 && time_us < 150000) play_until(&voice, time_us + 1000);
    assert(written == 3 && write_tags[0] == 10 && write_tags[1] == 11 && write_tags[2] == 12);
    puts("PASS starts at the earliest buffered speech even if START is lost");
}

static void test_capture_recovers_hardware_backlog(void)
{
    reset_audio();
    intercom_voice_t voice = {.mode = VOICE_TX};
    /* The fake ADC already has two frames buffered. Consecutive capture
     * calls must drain them without waiting for a second wall clock. */
    time_us = 1000;
    capture_step(&voice, (void *)1, 1, time_us);
    capture_step(&voice, (void *)1, 1, time_us + 1000);
    assert(captured == 2 && radio_audio == 2);

    key_released = true;
    capture_step(&voice, (void *)1, 1, 3000);
    capture_step(&voice, (void *)1, 1, 3000 + KEY_RELEASE_US - 1);
    assert(voice.mode == VOICE_TX && captured == 2 && radio_stops == 0);
    capture_step(&voice, (void *)1, 1, 3000 + KEY_RELEASE_US);
    assert(voice.mode == VOICE_IDLE && captured == 2 && radio_stops == 1);
    puts("PASS drains already available ADC frames and sends no new audio after release");
}

static void test_push_to_talk_sources(void)
{
    reset_audio();
    intercom_voice_t voice = {0};
    begin_capture(&voice, (void *)1, 1, false);
    assert(voice.mode == VOICE_TX && !voice.screen_talk && radio_starts == 1);
    screen_held = true;
    key_released = true;
    capture_step(&voice, (void *)1, 1, 3000);
    capture_step(&voice, (void *)1, 1, 3000 + KEY_RELEASE_US);
    assert(voice.mode == VOICE_IDLE && radio_stops == 1 && !captured);

    begin_capture(&voice, (void *)1, 1, true);
    assert(voice.mode == VOICE_TX && voice.screen_talk && radio_starts == 2);
    capture_step(&voice, (void *)1, 1, 4000000);
    assert(voice.mode == VOICE_TX && captured == 1 && radio_audio == 1);
    screen_held = false;
    key_released = false;
    capture_step(&voice, (void *)1, 1, 4020000);
    assert(voice.mode == VOICE_IDLE && radio_stops == 2 && captured == 1);

    screen_held = release_on_capture_start = true;
    begin_capture(&voice, (void *)1, 1, true);
    assert(capture_starts == 3 && voice.mode == VOICE_IDLE && radio_starts == 2);
    release_on_capture_start = false;
    screen_held = true;
    begin_capture(&voice, (void *)1, 2, true);
    assert(voice.mode == VOICE_IDLE && radio_starts == 2);
    puts("PASS physical and screen PTT start while held and stop with their own release; stale/released startup cannot transmit");
}

int main(void)
{
    test_arbitration_and_short_burst();
    test_forward_rebase();
    test_prebuffer_and_rebuffer();
    test_first_audio_anchors_startup_limit();
    test_receive_rewinds_before_playing();
    test_capture_recovers_hardware_backlog();
    test_push_to_talk_sources();
    return 0;
}
