#pragma once

#include <stdint.h>

typedef struct esp_nsn_data_t esp_nsn_data_t;
typedef struct {
    esp_nsn_data_t *(*create)(char *);
    int (*get_samp_chunksize)(esp_nsn_data_t *);
    int (*process)(esp_nsn_data_t *, int16_t *, int16_t *);
    int (*get_samp_rate)(esp_nsn_data_t *);
    void (*destroy)(esp_nsn_data_t *);
    esp_nsn_data_t *(*create_mc)(char *, int, int);
    int (*process_mc)(esp_nsn_data_t *, int16_t **, int16_t **);
} esp_nsn_iface_t;
