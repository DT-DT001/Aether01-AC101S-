/* Optional real ESP32-S31 library acceptance; not linked to the normal app. */
#include "unity.h"
#include "sdkconfig.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "fixture.h"

#if !CONFIG_IDF_TARGET_ESP32S31
#error "Run this acceptance test on ESP32-S31, not a different ESP32 target"
#endif
#if defined(CONFIG_ESP_H264_S31_USE_ASM) && CONFIG_ESP_H264_S31_USE_ASM
#error "This baseline acceptance uses the official S31 portable-C decoder"
#endif
TEST_CASE("S31 official H264 original constrained baseline I_PCM", "[h264][mp4][s31]")
{
    esp_h264_dec_handle_t decoder=NULL;
    esp_h264_dec_cfg_sw_t config={.pic_type=ESP_H264_RAW_FMT_I420};
    TEST_ASSERT_EQUAL(ESP_H264_ERR_OK,esp_h264_dec_sw_new(&config,&decoder));
    TEST_ASSERT_EQUAL(ESP_H264_ERR_OK,esp_h264_dec_open(decoder));
    for(unsigned pass=0;pass<3;++pass) {
        size_t used=0; unsigned pictures=0;
        while(used<sizeof(MP4_H264_FIXTURE)) {
            esp_h264_dec_in_frame_t in={.raw_data={.buffer=(uint8_t *)MP4_H264_FIXTURE+used,.len=sizeof(MP4_H264_FIXTURE)-used}};
            esp_h264_dec_out_frame_t out={0};
            TEST_ASSERT_EQUAL(ESP_H264_ERR_OK,esp_h264_dec_process(decoder,&in,&out));
            TEST_ASSERT_TRUE(in.consume>0 && in.consume<=sizeof(MP4_H264_FIXTURE)-used);
            used+=in.consume;
            if(out.out_size) {
                TEST_ASSERT_EQUAL_UINT32(32*32*3/2,out.out_size);
                TEST_ASSERT_NOT_NULL(out.outbuf); ++pictures;
                for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x)
                    TEST_ASSERT_EQUAL_UINT8(16+((y/16)*2+x/16)*3,out.outbuf[y*32+x]);
                for(unsigned i=32*32;i<32*32*3/2;++i) TEST_ASSERT_EQUAL_UINT8(128,out.outbuf[i]);
            }
        }
        TEST_ASSERT_EQUAL_UINT32(1,pictures);
    }
    TEST_ASSERT_EQUAL(ESP_H264_ERR_OK,esp_h264_dec_close(decoder));
    TEST_ASSERT_EQUAL(ESP_H264_ERR_OK,esp_h264_dec_del(decoder));
}
