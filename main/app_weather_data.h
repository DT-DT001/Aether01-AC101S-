#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

enum { APP_WEATHER_DAYS = 3 };
typedef struct {
    char date[11];
    int min_c, max_c;
    unsigned weather_code;
} app_weather_day_t;

typedef struct {
    bool valid, fetching;
    unsigned city, weather_code, humidity;
    int temperature_c;
    char observed[17];
    app_weather_day_t daily[APP_WEATHER_DAYS];
    esp_err_t error;
} app_weather_snapshot_t;

unsigned app_weather_city_count(void);
const char *app_weather_city(unsigned city);
const char *app_weather_condition(unsigned code);
bool app_weather_forecast_url(unsigned city, char *url, size_t capacity);
bool app_weather_parse(const char *json, app_weather_snapshot_t *out);
