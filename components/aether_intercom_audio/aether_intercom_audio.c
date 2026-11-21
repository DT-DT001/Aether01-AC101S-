#include "aether_intercom_audio.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    VOICE_RATE_HZ = 16000,
    SAMPLE_COUNT = VOICE_RATE_HZ * AETHER_INTERCOM_AUDIO_FRAME_MS / 1000,
    RESAMPLE_RATIO = 3,
    CODEC_SAMPLES = SAMPLE_COUNT * RESAMPLE_RATIO,
    FILTER_TAPS = 31,
    FILTER_HISTORY = FILTER_TAPS - 1,
    PCM_TIMEOUT_MS = 60,
    HEADER_BYTES = 3,
    LAST_STEP_INDEX = 88,
    PCM_GAIN_Q12 = 4096,
    PCM_GAIN_NOMINAL = 2 * PCM_GAIN_Q12,
    PCM_VOICE_PEAK = 21000,
    PCM_GAIN_RAMP = 24,
    PLC_MIN_PERIOD = VOICE_RATE_HZ / 500,
    PLC_MAX_PERIOD = VOICE_RATE_HZ / 80,
    PLC_COMPARE = 96,
    PLC_CROSSFADE = 24,
    PLC_TAIL = 48,
};

_Static_assert(AETHER_INTERCOM_AUDIO_FRAME_BYTES == HEADER_BYTES + SAMPLE_COUNT / 2,
               "ADPCM frame size must match the voice sample count");

struct aether_intercom_audio {
    ac101s_audio_handle_t codec;
    aether_intercom_audio_mode_t mode;
    bool active;
    bool stop_pending;
    bool playback_history_valid;
    int16_t playback_history;
    bool capture_dc_valid;
    int32_t capture_dc_q8;
    int32_t capture_gain_q12;
    int capture_index;
    bool received_samples;
    unsigned concealed_frames;
    unsigned plc_period;
    unsigned plc_phase;
    int16_t stereo[CODEC_SAMPLES][2];
    int16_t capture_mono[FILTER_HISTORY + CODEC_SAMPLES];
    /* The two directions are exclusive. Capture uses this for a peak pass
     * before encoding; playback keeps the last authentic frame for loss. */
    int16_t voice[SAMPLE_COUNT];
};

/* Standard IMA ADPCM step and index tables. */
static const int16_t step_size[LAST_STEP_INDEX + 1] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const int8_t step_change[] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* Unity gain in Q15. The 6 kHz low-pass reduces aliasing before dropping
 * two out of every three 48 kHz microphone samples. */
static const int16_t downsample_taps[FILTER_TAPS] = {
    -39, -67, -68, 0, 156, 324, 327, 0, -621, -1189, -1139, 0, 2249, 5022, 7322,
    8214, 7322, 5022, 2249, 0, -1139, -1189, -621, 0, 327, 324, 156, 0, -68, -67, -39,
};

static int16_t clip_sample(int32_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static int16_t adpcm_decode_sample(unsigned code, int16_t predictor, int *index)
{
    const int32_t step = step_size[*index];
    int32_t delta = step >> 3;
    if (code & 4U) delta += step;
    if (code & 2U) delta += step >> 1;
    if (code & 1U) delta += step >> 2;
    int next = *index + step_change[code & 7U];
    if (next < 0) next = 0;
    if (next > LAST_STEP_INDEX) next = LAST_STEP_INDEX;
    *index = next;
    return clip_sample((int32_t)predictor + ((code & 8U) ? -delta : delta));
}

static unsigned adpcm_encode_sample(int16_t sample, int16_t predictor, int index)
{
    int32_t delta = (int32_t)sample - predictor;
    unsigned code = 0;
    if (delta < 0) {
        delta = -delta;
        code = 8U;
    }
    int32_t step = step_size[index];
    for (unsigned bit = 4U; bit; bit >>= 1, step >>= 1) {
        if (delta >= step) {
            delta -= step;
            code |= bit;
        }
    }
    return code;
}

static int16_t downsample(const aether_intercom_audio_handle_t h, unsigned sample)
{
    const int16_t *input = h->capture_mono + RESAMPLE_RATIO * sample;
    int32_t accumulator = 0;
    for (unsigned tap = 0; tap < FILTER_TAPS; ++tap) {
        accumulator += (int32_t)input[tap] * downsample_taps[tap];
    }
    return clip_sample(accumulator / 32768);
}

static int16_t remove_capture_dc(aether_intercom_audio_handle_t h, int16_t sample)
{
    const int32_t q8 = (int32_t)sample * 256;
    if (!h->capture_dc_valid) {
        h->capture_dc_q8 = q8;
        h->capture_dc_valid = true;
    }
    /* About 80 Hz at 16 kHz. Leave voiced fundamentals and suppress
     * handling noise/DC before either peak detection or ADPCM coding. */
    h->capture_dc_q8 += (q8 - h->capture_dc_q8) / 32;
    return clip_sample((q8 - h->capture_dc_q8) / 256);
}

static int16_t scale_voice(int16_t sample, int32_t gain_q12)
{
    const int32_t pcm = (int32_t)sample * gain_q12 / PCM_GAIN_Q12;
    const int32_t peak = pcm < 0 ? -pcm : pcm;
    if (peak <= PCM_VOICE_PEAK) return (int16_t)pcm;
    /* The frame limiter has a short gain ramp. Round off any remaining
     * overshoot during its attack instead of hard clipping the waveform. */
    const int32_t over = peak - PCM_VOICE_PEAK;
    const int32_t softened = PCM_VOICE_PEAK + over * 9000 / (over + 9000);
    return (int16_t)(pcm < 0 ? -softened : softened);
}

static void prepare_capture(aether_intercom_audio_handle_t h)
{
    int32_t peak = 1;
    for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
        const int32_t sample = remove_capture_dc(h, downsample(h, i));
        h->voice[i] = (int16_t)sample;
        const int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
    }
    const int32_t old_gain = h->capture_gain_q12;
    int32_t target = PCM_VOICE_PEAK * PCM_GAIN_Q12 / peak;
    if (target > PCM_GAIN_NOMINAL) target = PCM_GAIN_NOMINAL;
    if (target > old_gain) target = old_gain + (target - old_gain + 7) / 8;
    for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
        const int32_t gain = i < PCM_GAIN_RAMP ?
            old_gain + (target - old_gain) * (int32_t)(i + 1) / PCM_GAIN_RAMP : target;
        h->voice[i] = scale_voice(h->voice[i], gain);
    }
    h->capture_gain_q12 = target;
}

static void set_playback_sample(aether_intercom_audio_handle_t h, unsigned sample, int16_t value)
{
    const int32_t previous = h->playback_history_valid ? h->playback_history : value;
    for (int phase = 1; phase <= RESAMPLE_RATIO; ++phase) {
        const unsigned i = RESAMPLE_RATIO * sample + (unsigned)(phase - 1);
        const int16_t lerp = (int16_t)((previous * (RESAMPLE_RATIO - phase) + (int32_t)value * phase) / RESAMPLE_RATIO);
        h->stereo[i][0] = lerp;
        h->stereo[i][1] = lerp;
    }
    h->playback_history = value;
    h->playback_history_valid = true;
}

static unsigned find_voice_period(const int16_t samples[SAMPLE_COUNT])
{
    const unsigned first = SAMPLE_COUNT - PLC_COMPARE;
    int32_t signal = 0;
    for (unsigned i = first; i < SAMPLE_COUNT; i += 2) {
        const int32_t sample = samples[i];
        signal += sample < 0 ? -sample : sample;
    }
    if (signal < 160 * (PLC_COMPARE / 2)) return 0;

    unsigned period = 0;
    int32_t best = signal;
    for (unsigned lag = PLC_MIN_PERIOD; lag <= PLC_MAX_PERIOD; ++lag) {
        int32_t error = 0;
        for (unsigned i = first; i < SAMPLE_COUNT; i += 2) {
            const int32_t delta = (int32_t)samples[i] - samples[i - lag];
            error += delta < 0 ? -delta : delta;
        }
        /* A small bias towards the shorter period prevents selecting
         * near-multiples of the fundamental when the scores are close. */
        error += signal * (int32_t)lag / 2048;
        if (error < best) {
            best = error;
            period = lag;
        }
    }
    return best * 3 < signal * 2 ? period : 0;
}

static int16_t blend_samples(int16_t from, int16_t to, unsigned sample, unsigned length)
{
    return (int16_t)(((int32_t)from * (int32_t)(length - sample) +
                      (int32_t)to * (int32_t)sample) / (int32_t)length);
}

esp_err_t aether_intercom_audio_new(ac101s_audio_handle_t codec, aether_intercom_audio_handle_t *out)
{
    if (!codec || !out) return ESP_ERR_INVALID_ARG;
    *out = calloc(1, sizeof(**out));
    if (!*out) return ESP_ERR_NO_MEM;
    (*out)->codec = codec;
    return ESP_OK;
}

esp_err_t aether_intercom_audio_start(aether_intercom_audio_handle_t h, aether_intercom_audio_mode_t mode)
{
    if (!h || (mode != AETHER_INTERCOM_AUDIO_CAPTURE && mode != AETHER_INTERCOM_AUDIO_PLAYBACK)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->stop_pending) return ESP_ERR_INVALID_STATE;
    h->stop_pending = true;
    /* Leave an extra 6 dB of analogue headroom for close microphone speech.
     * The digital gain restores normal levels with a limiter before ADPCM. */
    const esp_err_t error = ac101s_audio_board_start(h->codec, h, mode == AETHER_INTERCOM_AUDIO_CAPTURE,
                                                    100, 18.0f);
    if (error != ESP_OK) return error;
    h->mode = mode;
    h->active = true;
    h->capture_index = 0;
    h->capture_dc_valid = false;
    h->capture_gain_q12 = PCM_GAIN_NOMINAL;
    memset(h->capture_mono, 0, FILTER_HISTORY * sizeof(h->capture_mono[0]));
    h->playback_history_valid = false;
    h->received_samples = false;
    h->concealed_frames = 0;
    return ESP_OK;
}

esp_err_t aether_intercom_audio_stop(aether_intercom_audio_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (!h->stop_pending) return ESP_OK;
    h->active = false;
    const esp_err_t error = ac101s_audio_board_stop(h->codec, h);
    if (error == ESP_OK) h->stop_pending = false;
    return error;
}

esp_err_t aether_intercom_audio_del(aether_intercom_audio_handle_t h)
{
    if (!h) return ESP_OK;
    const esp_err_t error = aether_intercom_audio_stop(h);
    if (error == ESP_OK) free(h);
    return error;
}

esp_err_t aether_intercom_audio_capture(aether_intercom_audio_handle_t h,
                                       uint8_t frame[AETHER_INTERCOM_AUDIO_FRAME_BYTES])
{
    if (!h || !frame) return ESP_ERR_INVALID_ARG;
    if (!h->active || h->mode != AETHER_INTERCOM_AUDIO_CAPTURE) return ESP_ERR_INVALID_STATE;
    const esp_err_t error = ac101s_audio_board_pcm(h->codec, h, h->stereo, sizeof(h->stereo), PCM_TIMEOUT_MS);
    if (error != ESP_OK) return error;

    for (unsigned i = 0; i < CODEC_SAMPLES; ++i) {
        h->capture_mono[FILTER_HISTORY + i] = h->stereo[i][0];
    }
    prepare_capture(h);
    memset(frame + HEADER_BYTES, 0, AETHER_INTERCOM_AUDIO_FRAME_BYTES - HEADER_BYTES);
    int16_t predictor = h->voice[0];
    const uint16_t first_sample = (uint16_t)predictor;
    frame[0] = (uint8_t)first_sample;
    frame[1] = (uint8_t)(first_sample >> 8);
    frame[2] = (uint8_t)h->capture_index;
    for (unsigned i = 1; i < SAMPLE_COUNT; ++i) {
        const unsigned code = adpcm_encode_sample(h->voice[i], predictor, h->capture_index);
        predictor = adpcm_decode_sample(code, predictor, &h->capture_index);
        const unsigned delta = i - 1;
        frame[HEADER_BYTES + delta / 2] |= (uint8_t)(code << (4U * (delta & 1U)));
    }
    memmove(h->capture_mono, h->capture_mono + CODEC_SAMPLES, FILTER_HISTORY * sizeof(h->capture_mono[0]));
    return ESP_OK;
}

esp_err_t aether_intercom_audio_play(aether_intercom_audio_handle_t h,
                                    const uint8_t frame[AETHER_INTERCOM_AUDIO_FRAME_BYTES])
{
    if (!h || !frame || frame[2] > LAST_STEP_INDEX) return ESP_ERR_INVALID_ARG;
    if (!h->active || h->mode != AETHER_INTERCOM_AUDIO_PLAYBACK) return ESP_ERR_INVALID_STATE;
    int index = frame[2];
    int32_t first = frame[0] | (int32_t)frame[1] << 8;
    if (first > INT16_MAX) first -= 65536;
    h->voice[0] = (int16_t)first;
    for (unsigned i = 1; i < SAMPLE_COUNT; ++i) {
        const unsigned delta = i - 1;
        const unsigned code = ((unsigned)frame[HEADER_BYTES + delta / 2] >> (4U * (delta & 1U))) & 15U;
        h->voice[i] = adpcm_decode_sample(code, h->voice[i - 1], &index);
    }
    const int16_t last = h->playback_history;
    for (unsigned i = 0; i < SAMPLE_COUNT; ++i)
        set_playback_sample(h, i, h->concealed_frames && i < PLC_CROSSFADE ?
                            blend_samples(last, h->voice[i], i + 1, PLC_CROSSFADE) : h->voice[i]);
    h->received_samples = true;
    h->concealed_frames = 0;
    return ac101s_audio_board_pcm(h->codec, h, h->stereo, sizeof(h->stereo), PCM_TIMEOUT_MS);
}

esp_err_t aether_intercom_audio_conceal(aether_intercom_audio_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (!h->active || h->mode != AETHER_INTERCOM_AUDIO_PLAYBACK) return ESP_ERR_INVALID_STATE;
    const unsigned loss = h->concealed_frames;
    if (loss < 3) ++h->concealed_frames;
    if (!h->received_samples || loss >= 3) {
        memset(h->stereo, 0, sizeof(h->stereo));
        h->playback_history = 0;
        h->playback_history_valid = true;
    } else {
        static const unsigned gain_percent[] = {100, 58, 21, 0};
        if (loss == 0) {
            h->plc_period = find_voice_period(h->voice);
            h->plc_phase = 0;
        }
        const int16_t last = h->playback_history;
        const unsigned period = h->plc_period;
        for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
            int16_t value = 0;
            if (period) {
                const unsigned percent = (gain_percent[loss] * (SAMPLE_COUNT - i - 1) +
                                          gain_percent[loss + 1] * (i + 1)) / SAMPLE_COUNT;
                const int16_t repeated = h->voice[SAMPLE_COUNT - period + h->plc_phase];
                value = (int16_t)((int32_t)repeated * (int32_t)percent / 100);
                if (++h->plc_phase == period) h->plc_phase = 0;
                if (i < PLC_CROSSFADE) value = blend_samples(last, value, i + 1, PLC_CROSSFADE);
            } else if (i < PLC_TAIL) {
                value = blend_samples(last, 0, i + 1, PLC_TAIL);
            }
            set_playback_sample(h, i, value);
        }
    }
    return ac101s_audio_board_pcm(h->codec, h, h->stereo, sizeof(h->stereo), PCM_TIMEOUT_MS);
}

esp_err_t aether_intercom_audio_silence(aether_intercom_audio_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (!h->active || h->mode != AETHER_INTERCOM_AUDIO_PLAYBACK) return ESP_ERR_INVALID_STATE;
    memset(h->stereo, 0, sizeof(h->stereo));
    /* Bring the previous sample smoothly to zero at a missing frame. */
    if (h->playback_history_valid) set_playback_sample(h, 0, 0);
    h->playback_history = 0;
    h->playback_history_valid = true;
    h->received_samples = false;
    h->concealed_frames = 0;
    return ac101s_audio_board_pcm(h->codec, h, h->stereo, sizeof(h->stereo), PCM_TIMEOUT_MS);
}
