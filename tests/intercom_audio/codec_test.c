#include "aether_intercom_audio.h"

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

enum { VOICE_SAMPLES = 320, PCM_FRAMES = VOICE_SAMPLES * 3 };

static struct {
    const void *owner;
    bool capture;
    unsigned capture_offset;
    unsigned writes;
    bool fail_stop;
    bool loud;
    int16_t playback[PCM_FRAMES][2];
} fake;

static int16_t input_sample(int index)
{
    if (index < 0) return 0;
    const int phase = index % 96;
    const int speech = (phase < 48 ? phase : 96 - phase) * 416 - 9984;
    return (int16_t)(fake.loud ? speech * 2 + 9000 : speech);
}

esp_err_t ac101s_audio_board_start(ac101s_audio_handle_t audio, const void *owner, bool capture,
                                  int volume, float gain_db)
{
    if (!audio || !owner || volume != 100 || gain_db != 18.0f) return ESP_ERR_INVALID_ARG;
    if (fake.owner) return ESP_ERR_INVALID_STATE;
    fake.owner = owner;
    fake.capture = capture;
    return ESP_OK;
}

esp_err_t ac101s_audio_board_pcm(ac101s_audio_handle_t audio, const void *owner,
                                void *pcm, size_t bytes, uint32_t timeout_ms)
{
    if (!audio || !owner || fake.owner != owner || bytes != sizeof(fake.playback) || timeout_ms != 60)
        return ESP_ERR_INVALID_ARG;
    int16_t (*samples)[2] = pcm;
    for (unsigned i = 0; i < PCM_FRAMES; ++i) {
        if (fake.capture) {
            samples[i][0] = input_sample((int)(fake.capture_offset + i));
            samples[i][1] = 0;
        } else {
            fake.playback[i][0] = samples[i][0];
            fake.playback[i][1] = samples[i][1];
        }
    }
    if (fake.capture) fake.capture_offset += PCM_FRAMES;
    else ++fake.writes;
    return ESP_OK;
}

esp_err_t ac101s_audio_board_stop(ac101s_audio_handle_t audio, const void *owner)
{
    if (!audio || !owner) return ESP_ERR_INVALID_ARG;
    if (fake.owner == owner) {
        if (fake.fail_stop) return ESP_FAIL;
        fake.owner = NULL;
    }
    return ESP_OK;
}

int main(void)
{
    static int driver_token;
    ac101s_audio_handle_t physical_codec = (ac101s_audio_handle_t)&driver_token;
    aether_intercom_audio_handle_t h = NULL;
    uint8_t frames[3][AETHER_INTERCOM_AUDIO_FRAME_BYTES];
    uint8_t loud_frame[AETHER_INTERCOM_AUDIO_FRAME_BYTES];
    CHECK(aether_intercom_audio_new(physical_codec, &h) == ESP_OK);
    CHECK(aether_intercom_audio_start(h, AETHER_INTERCOM_AUDIO_CAPTURE) == ESP_OK);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(aether_intercom_audio_capture(h, frames[i]) == ESP_OK);
        CHECK(frames[i][2] <= 88 && (frames[i][AETHER_INTERCOM_AUDIO_FRAME_BYTES - 1] & 0xf0) == 0);
    }
    CHECK(fake.writes == 0);
    fake.fail_stop = true;
    CHECK(aether_intercom_audio_stop(h) == ESP_FAIL);
    CHECK(aether_intercom_audio_start(h, AETHER_INTERCOM_AUDIO_PLAYBACK) == ESP_ERR_INVALID_STATE);
    CHECK(aether_intercom_audio_capture(h, frames[0]) == ESP_ERR_INVALID_STATE);
    fake.fail_stop = false;
    CHECK(aether_intercom_audio_stop(h) == ESP_OK);

    fake.loud = true;
    CHECK(aether_intercom_audio_start(h, AETHER_INTERCOM_AUDIO_CAPTURE) == ESP_OK);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(aether_intercom_audio_capture(h, loud_frame) == ESP_OK);
    CHECK(aether_intercom_audio_stop(h) == ESP_OK);
    fake.loud = false;

    CHECK(aether_intercom_audio_start(h, AETHER_INTERCOM_AUDIO_PLAYBACK) == ESP_OK);
    CHECK(aether_intercom_audio_play(h, frames[0]) == ESP_OK);
    /* Drop frame 1. The next speech packet is self-contained. The capture
     * filters, digital gain and ADPCM still preserve a 500 Hz voice tone. */
    CHECK(aether_intercom_audio_play(h, frames[2]) == ESP_OK);
    static int16_t decoded[PCM_FRAMES];
    int total_error = 0;
    for (unsigned i = 0; i < VOICE_SAMPLES; ++i) {
        const unsigned output = 3 * i + 2;
        CHECK(fake.playback[output][0] == fake.playback[output][1]);
        const int error = fake.playback[output][0] - 2 * input_sample(2 * PCM_FRAMES + (int)(3 * i) - 12);
        total_error += error < 0 ? -error : error;
        decoded[output] = fake.playback[output][0];
    }
    CHECK(total_error / VOICE_SAMPLES < 3200);
    CHECK(aether_intercom_audio_stop(h) == ESP_OK);
    CHECK(aether_intercom_audio_start(h, AETHER_INTERCOM_AUDIO_PLAYBACK) == ESP_OK);
    CHECK(aether_intercom_audio_play(h, frames[2]) == ESP_OK);
    for (unsigned i = 0; i < VOICE_SAMPLES; ++i) {
        const unsigned output = 3 * i + 2;
        CHECK(decoded[output] == fake.playback[output][0]);
    }

    unsigned loss_energy[4] = {0};
    for (unsigned loss = 0; loss < 4; ++loss) {
        CHECK(aether_intercom_audio_conceal(h) == ESP_OK);
        for (unsigned i = 0; i < VOICE_SAMPLES; ++i) {
            const int sample = fake.playback[3 * i + 2][0];
            loss_energy[loss] += (unsigned)(sample < 0 ? -sample : sample);
        }
    }
    CHECK(loss_energy[0] > 2000 * VOICE_SAMPLES);
    CHECK(loss_energy[0] > loss_energy[1] && loss_energy[1] > loss_energy[2] && loss_energy[3] == 0);
    CHECK(aether_intercom_audio_play(h, frames[2]) == ESP_OK);
    CHECK(fake.playback[2][0] > -1000 && fake.playback[2][0] < 1000);
    CHECK(fake.playback[3 * 24 + 2][0] == decoded[3 * 24 + 2]);

    CHECK(aether_intercom_audio_play(h, loud_frame) == ESP_OK);
    int loud_mean = 0;
    int loud_peak = 0;
    for (unsigned i = 0; i < VOICE_SAMPLES; ++i) {
        const int sample = fake.playback[3 * i + 2][0];
        const int magnitude = sample < 0 ? -sample : sample;
        loud_mean += sample;
        if (magnitude > loud_peak) loud_peak = magnitude;
    }
    CHECK(loud_mean / VOICE_SAMPLES > -1000 && loud_mean / VOICE_SAMPLES < 1000);
    CHECK(loud_peak > 18000 && loud_peak < 28000);

    /* Signed little-endian predictor, then low nibble followed by high.
     * With step indices 0, 8, 16, codes 7, F, 0 decode deltas +11, -30, +4. */
    const uint8_t vector[AETHER_INTERCOM_AUDIO_FRAME_BYTES] = {0x18, 0xfc, 0x00, 0xf7, 0x00};
    CHECK(aether_intercom_audio_play(h, vector) == ESP_OK);
    CHECK(fake.playback[2][0] == -1000 && fake.playback[5][0] == -989);
    CHECK(fake.playback[8][0] == -1019 && fake.playback[11][0] == -1015);
    const unsigned writes = fake.writes;
    frames[1][2] = 89;
    CHECK(aether_intercom_audio_play(h, frames[1]) == ESP_ERR_INVALID_ARG && fake.writes == writes);
    CHECK(aether_intercom_audio_silence(h) == ESP_OK);
    for (unsigned i = 2; i < PCM_FRAMES; ++i) {
        CHECK(fake.playback[i][0] == 0 && fake.playback[i][1] == 0);
    }
    CHECK(aether_intercom_audio_del(h) == ESP_OK && fake.owner == NULL);
    return 0;
}
