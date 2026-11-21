#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "app_tools_math.h"

static void test_calendar(void)
{
    assert(app_calendar_days(2000, 2) == 29);
    assert(app_calendar_days(2024, 2) == 29);
    assert(app_calendar_days(2025, 2) == 28);
    assert(app_calendar_days(2100, 2) == 28);
    assert(app_calendar_days(2026, 0) == 0);
    assert(!app_calendar_valid(2025, 2, 29, 0, 0, 0));
    assert(!app_calendar_valid(2026, 4, 31, 0, 0, 0));
    assert(!app_calendar_valid(2026, 1, 1, 24, 0, 0));
    assert(!app_calendar_valid(2100, 1, 1, 0, 0, 0));
    assert(app_calendar_valid(2024, 2, 29, 23, 59, 59));
    assert(app_calendar_weekday(2000, 1, 1) == 6);
    assert(app_calendar_weekday(2026, 11, 3) == 2);
    assert(app_calendar_seconds(2000, 1, 1, 0, 0, 0) == 0);
    assert(app_calendar_seconds(2024, 3, 1, 0, 0, 0) -
           app_calendar_seconds(2024, 2, 28, 0, 0, 0) == 2 * 86400);
    assert(app_calendar_seconds(2026, 12, 31, 23, 59, 59) + 1 ==
           app_calendar_seconds(2027, 1, 1, 0, 0, 0));
    assert(app_calendar_seconds(2026, 2, 30, 0, 0, 0) == UINT64_MAX);
}

static void test_pressure(void)
{
    assert(fabsf(app_pressure_altitude(101325, 101325)) < 0.01f);
    const float elevated = app_pressure_altitude(89875, 101325);
    assert(elevated > 990 && elevated < 1010);
    assert(app_pressure_altitude(110000, 101325) < 0);
    assert(isnan(app_pressure_altitude(0, 101325)));
    assert(isnan(app_pressure_altitude(100000, 0)));
    assert(isnan(app_pressure_altitude(NAN, 101325)));
}

int main(void)
{
    test_calendar();
    test_pressure();
    puts("tools math: calendar and altitude passed");
    return 0;
}
