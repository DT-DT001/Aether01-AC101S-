#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AETHER_IMAGE_MAX_JPEG_BYTES (8U * 1024U * 1024U)

typedef struct aether_image_decoder aether_image_decoder_t;

/* Context and its DMA scratch are owned by the caller's worker task. */
esp_err_t aether_image_decoder_new(aether_image_decoder_t **out);
void aether_image_decoder_del(aether_image_decoder_t *decoder);

/* Decode a baseline JPEG into a tightly packed native RGB565 image that fits
 * the supplied bounds at its original aspect ratio, with no upscaling.
 * rgb565 must hold max_width * max_height pixels. */
esp_err_t aether_image_decode_jpeg(aether_image_decoder_t *decoder, const uint8_t *jpeg,
                                  size_t jpeg_bytes, uint16_t *rgb565, uint16_t max_width,
                                  uint16_t max_height, uint16_t *out_width,
                                  uint16_t *out_height);
