#pragma once
#include <stdbool.h>
typedef struct { bool valid; unsigned city, weather_code; int temperature_c; } app_weather_snapshot_t;
void app_weather_get(app_weather_snapshot_t *);
void app_weather_refresh(bool);
const char *app_weather_city(unsigned);
const char *app_weather_condition(unsigned);
