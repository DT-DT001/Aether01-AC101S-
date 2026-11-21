/* Optional ESP-IDF Unity component: actual esp_audio_codec on ESP32-S31.
 * This is not part of the normal application and is not a host decoder mock. */
#include "unity.h"
#include "sdkconfig.h"
#include "../../../components/aether_mp4_player/mp4_aac.h"
#include "../fixture.h"

#if !CONFIG_IDF_TARGET_ESP32S31
#error "This acceptance case is intended for the actual ESP32-S31 target"
#endif
#if defined(CONFIG_ESP_AUDIO_CODEC_S31_USE_ASM) && CONFIG_ESP_AUDIO_CODEC_S31_USE_ASM
#error "Use esp_audio_codec 2.6.2 ESP32-S31 portable C, ASM disabled"
#endif

static esp_err_t silent_pcm(const int16_t *pcm, size_t frames, void *context)
{
    TEST_ASSERT_GREATER_THAN_UINT32(0, frames);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1024, frames);
    for (size_t i = 0; i < frames * 2U; ++i) TEST_ASSERT_EQUAL_INT16(0, pcm[i]);
    *(uint64_t *)context += frames;
    return ESP_OK;
}

TEST_CASE("S31 official AAC-LC mono/stereo original fixture", "[aac][s31]")
{
    for (unsigned channels = 1; channels <= 2; ++channels) {
        const uint8_t *asc = channels == 1 ? AAC_FIXTURE_MONO_ASC : AAC_FIXTURE_STEREO_ASC;
        const uint8_t *au = channels == 1 ? AAC_FIXTURE_MONO_AU : AAC_FIXTURE_STEREO_AU;
        const size_t length = channels == 1 ? sizeof(AAC_FIXTURE_MONO_AU) : sizeof(AAC_FIXTURE_STEREO_AU);
        mp4_aac_handle_t decoder = NULL;
        TEST_ASSERT_EQUAL(ESP_OK, mp4_aac_new(asc, 2, NULL, NULL, &decoder));
        TEST_ASSERT_NOT_NULL(decoder);
        for (unsigned pass = 0; pass < 2; ++pass) {
            uint64_t frames = 0;
            for (unsigned frame = 0; frame < AAC_FIXTURE_AUS; ++frame)
                TEST_ASSERT_EQUAL(ESP_OK, mp4_aac_decode(decoder, au, length, silent_pcm, &frames));
            TEST_ASSERT_EQUAL(ESP_OK, mp4_aac_finish(decoder, silent_pcm, &frames));
            const uint64_t expected = ((uint64_t)AAC_FIXTURE_AUS * 1024U * 48000U + 44100U - 1U) / 44100U;
            /* IDF Unity defaults its optional uint64 formatting off. Keep
             * actual uint64 comparisons without requiring that setting. */
            TEST_ASSERT_TRUE(frames == expected);
            mp4_aac_status_t status;
            TEST_ASSERT_EQUAL(ESP_OK, mp4_aac_get_status(decoder, &status));
            TEST_ASSERT_TRUE(status.access_units == AAC_FIXTURE_AUS);
            TEST_ASSERT_TRUE(status.input_frames == AAC_FIXTURE_AUS * 1024U);
            TEST_ASSERT_TRUE(status.output_frames == expected);
            TEST_ASSERT_EQUAL_UINT32(44100, status.sample_rate);
            TEST_ASSERT_EQUAL_UINT8(channels, status.channels);
            TEST_ASSERT_TRUE(status.finished);
            TEST_ASSERT_EQUAL(ESP_OK, mp4_aac_reset(decoder));
        }
        mp4_aac_del(decoder);
    }
}
