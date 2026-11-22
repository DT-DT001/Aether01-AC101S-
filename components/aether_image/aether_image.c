#include "aether_image.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "driver/jpeg_decode.h"

#define MAX_DIMENSION 8192U
#define MAX_DECODED_PIXELS (8U * 1024U * 1024U)

struct aether_image_decoder {
    jpeg_decoder_handle_t engine;
    uint8_t *input;
    uint8_t *output;
    size_t input_capacity;
    size_t output_capacity;
};

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint16_t padded_height;
    size_t size_offset;
    bool grayscale;
} jpeg_image_info_t;

/* The hardware returns MCU-padded rows. Parse SOF0 to determine their stride
 * and keep the original visible rectangle for the final resize. */
static esp_err_t read_frame_info(const uint8_t *jpeg, size_t bytes, jpeg_image_info_t *info)
{
    if (bytes < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return ESP_ERR_INVALID_RESPONSE;
    size_t pos = 2;
    while (bytes - pos >= 4) {
        if (jpeg[pos++] != 0xff) return ESP_ERR_INVALID_RESPONSE;
        while (pos < bytes && jpeg[pos] == 0xff) ++pos;
        if (bytes - pos < 3) break;
        const uint8_t marker = jpeg[pos++];
        if (marker == 0xd9 || marker == 0xda) break;
        const size_t length = (uint16_t)jpeg[pos] << 8 | jpeg[pos + 1];
        if (length < 2 || length > bytes - pos) return ESP_ERR_INVALID_RESPONSE;
        if (marker == 0xc0) {
            if (length < 11 || jpeg[pos + 2] != 8) return ESP_ERR_INVALID_RESPONSE;
            const uint8_t components = jpeg[pos + 7];
            const uint8_t sampling = jpeg[pos + 9];
            if (components != 1 && components != 3) return ESP_ERR_NOT_SUPPORTED;
            if (length != 8U + 3U * components) return ESP_ERR_INVALID_RESPONSE;
            if (sampling != 0x11 && sampling != 0x21 && sampling != 0x22) return ESP_ERR_NOT_SUPPORTED;
            const unsigned width = (uint16_t)jpeg[pos + 5] << 8 | jpeg[pos + 6];
            const unsigned height = (uint16_t)jpeg[pos + 3] << 8 | jpeg[pos + 4];
            if (!width || !height || width > MAX_DIMENSION || height > MAX_DIMENSION)
                return ESP_ERR_INVALID_SIZE;
            const unsigned mcu_width = (sampling >> 4) * 8U;
            const unsigned mcu_height = (sampling & 15U) * 8U;
            const unsigned pitch = (width + mcu_width - 1U) & ~(mcu_width - 1U);
            const unsigned padded_height = (height + mcu_height - 1U) & ~(mcu_height - 1U);
            if (pitch * padded_height > MAX_DECODED_PIXELS) return ESP_ERR_INVALID_SIZE;
            *info = (jpeg_image_info_t){
                .width = (uint16_t)width, .height = (uint16_t)height, .pitch = (uint16_t)pitch,
                .padded_height = (uint16_t)padded_height, .size_offset = pos + 3,
                .grayscale = components == 1,
            };
            return ESP_OK;
        }
        pos += length;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool grow_dma_buffer(uint8_t **buffer, size_t *capacity, size_t required,
                            jpeg_dec_buffer_alloc_direction_t direction)
{
    if (*buffer && *capacity >= required) return true;
    free(*buffer);
    /* Leave headroom for varying MJPEG frame sizes when streaming. */
    if (direction == JPEG_DEC_ALLOC_INPUT_BUFFER && required < 1024U * 1024U)
        required += required / 4U;
    const jpeg_decode_memory_alloc_cfg_t cfg = {.buffer_direction = direction};
    *buffer = jpeg_alloc_decoder_mem(required, &cfg, capacity);
    return *buffer != NULL;
}

esp_err_t aether_image_decoder_new(aether_image_decoder_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    aether_image_decoder_t *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return ESP_ERR_NO_MEM;
    const jpeg_decode_engine_cfg_t cfg = {.timeout_ms = 180};
    const esp_err_t error = jpeg_new_decoder_engine(&cfg, &decoder->engine);
    if (error != ESP_OK) {
        free(decoder);
        return error;
    }
    *out = decoder;
    return ESP_OK;
}

void aether_image_decoder_del(aether_image_decoder_t *decoder)
{
    if (!decoder) return;
    jpeg_del_decoder_engine(decoder->engine);
    free(decoder->input);
    free(decoder->output);
    free(decoder);
}

esp_err_t aether_image_decode_jpeg(aether_image_decoder_t *decoder, const uint8_t *jpeg,
                                  size_t jpeg_bytes, uint16_t *rgb565, uint16_t max_width,
                                  uint16_t max_height, uint16_t *out_width,
                                  uint16_t *out_height)
{
    if (!decoder || !jpeg || !rgb565 || !max_width || !max_height || !out_width || !out_height)
        return ESP_ERR_INVALID_ARG;
    if (!jpeg_bytes || jpeg_bytes > AETHER_IMAGE_MAX_JPEG_BYTES) return ESP_ERR_INVALID_SIZE;

    jpeg_image_info_t info;
    esp_err_t error = read_frame_info(jpeg, jpeg_bytes, &info);
    if (error != ESP_OK) return error;
    const size_t decoded_bytes = (size_t)info.pitch * info.padded_height *
                                 (info.grayscale ? sizeof(uint8_t) : sizeof(uint16_t));
    if (!grow_dma_buffer(&decoder->input, &decoder->input_capacity, jpeg_bytes,
                         JPEG_DEC_ALLOC_INPUT_BUFFER) ||
        !grow_dma_buffer(&decoder->output, &decoder->output_capacity, decoded_bytes,
                         JPEG_DEC_ALLOC_OUTPUT_BUFFER))
        return ESP_ERR_NO_MEM;
    memcpy(decoder->input, jpeg, jpeg_bytes);
    if ((unsigned)info.width * info.height % 8U != 0) {
        /* The final MCUs already include the padded pixels. Make that size
         * explicit for the driver, which requires width * height to divide by 8. */
        uint8_t *size = decoder->input + info.size_offset;
        size[0] = (uint8_t)(info.padded_height >> 8);
        size[1] = (uint8_t)info.padded_height;
        size[2] = (uint8_t)(info.pitch >> 8);
        size[3] = (uint8_t)info.pitch;
    }
    const jpeg_decode_cfg_t cfg = {
        .output_format = info.grayscale ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t actual_bytes = 0;
    error = jpeg_decoder_process(decoder->engine, &cfg, decoder->input, (uint32_t)jpeg_bytes,
                                 decoder->output, (uint32_t)decoder->output_capacity, &actual_bytes);
    if (error != ESP_OK) return error;
    if (actual_bytes != decoded_bytes) return ESP_ERR_INVALID_SIZE;

    unsigned width = info.width < max_width ? info.width : max_width;
    unsigned height = (unsigned)info.height * width / info.width;
    if (height > max_height) {
        height = max_height;
        width = (unsigned)info.width * height / info.height;
    }
    if (!width) width = 1;
    if (!height) height = 1;
    const unsigned x_step = ((unsigned)info.width << 16) / width;
    const unsigned y_step = ((unsigned)info.height << 16) / height;
    const uint16_t *pixels = (const uint16_t *)decoder->output;
    unsigned source_y = y_step / 2;
    for (unsigned y = 0; y < height; ++y, source_y += y_step) {
        const unsigned offset = (source_y >> 16) * info.pitch;
        if (!info.grayscale && width == info.width) {
            memcpy(rgb565, pixels + offset, width * sizeof(*rgb565));
            rgb565 += width;
            continue;
        }
        unsigned source_x = x_step / 2;
        for (unsigned x = 0; x < width; ++x, source_x += x_step) {
            const unsigned index = offset + (source_x >> 16);
            if (info.grayscale) {
                const uint16_t value = decoder->output[index];
                *rgb565++ = (uint16_t)((value >> 3) << 11 | (value >> 2) << 5 | value >> 3);
            } else {
                *rgb565++ = pixels[index];
            }
        }
    }
    *out_width = (uint16_t)width;
    *out_height = (uint16_t)height;
    return ESP_OK;
}
