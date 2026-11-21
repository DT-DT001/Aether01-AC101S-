#pragma once

#include <stdbool.h>
#include <stdint.h>

unsigned app_calendar_days(unsigned year, unsigned month);
bool app_calendar_valid(unsigned year, unsigned month, unsigned day,
                        unsigned hour, unsigned minute, unsigned second);
unsigned app_calendar_weekday(unsigned year, unsigned month, unsigned day);
uint64_t app_calendar_seconds(unsigned year, unsigned month, unsigned day,
                              unsigned hour, unsigned minute, unsigned second);
float app_pressure_altitude(float pressure_pa, float reference_pa);

