#include "app_tools_math.h"

#include <math.h>

unsigned app_calendar_days(unsigned year, unsigned month)
{
    static const unsigned char days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) return 29;
    return days[month - 1];
}

bool app_calendar_valid(unsigned year, unsigned month, unsigned day,
                        unsigned hour, unsigned minute, unsigned second)
{
    return year >= 2000 && year <= 2099 && day >= 1 &&
           day <= app_calendar_days(year, month) && hour < 24 && minute < 60 && second < 60;
}

unsigned app_calendar_weekday(unsigned year, unsigned month, unsigned day)
{
    static const unsigned adjustment[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (!app_calendar_valid(year, month, day, 0, 0, 0)) return 0;
    if (month < 3) --year;
    return (year + year / 4 - year / 100 + year / 400 + adjustment[month - 1] + day) % 7;
}

uint64_t app_calendar_seconds(unsigned year, unsigned month, unsigned day,
                              unsigned hour, unsigned minute, unsigned second)
{
    if (!app_calendar_valid(year, month, day, hour, minute, second)) return UINT64_MAX;
    uint64_t days = 0;
    for (unsigned y = 2000; y < year; ++y) days += app_calendar_days(y, 2) == 29 ? 366 : 365;
    for (unsigned m = 1; m < month; ++m) days += app_calendar_days(year, m);
    days += day - 1;
    return ((days * 24 + hour) * 60 + minute) * 60 + second;
}

float app_pressure_altitude(float pressure_pa, float reference_pa)
{
    if (!isfinite(pressure_pa) || !isfinite(reference_pa) || pressure_pa <= 0 || reference_pa <= 0) return NAN;
    return 44330.0f * (1.0f - powf(pressure_pa / reference_pa, 0.19029495f));
}
