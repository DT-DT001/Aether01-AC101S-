#include "app_weather_data.h"
#include "app_tools_math.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name, *latitude, *longitude;
} cities[] = {
    {"上海", "31.2304", "121.4737"},
    {"北京", "39.9042", "116.4074"},
    {"天津", "39.0851", "117.1994"},
    {"重庆", "29.5630", "106.5516"},
    {"广州", "23.1291", "113.2644"},
    {"深圳", "22.5431", "114.0579"},
    {"杭州", "30.2741", "120.1551"},
    {"南京", "32.0603", "118.7969"},
    {"苏州", "31.2989", "120.5853"},
    {"福州", "26.0745", "119.2965"},
    {"厦门", "24.4798", "118.0894"},
    {"武汉", "30.5928", "114.3055"},
    {"长沙", "28.2282", "112.9388"},
    {"郑州", "34.7466", "113.6254"},
    {"济南", "36.6512", "117.1201"},
    {"青岛", "36.0671", "120.3826"},
    {"成都", "30.5728", "104.0668"},
    {"西安", "34.3416", "108.9398"},
    {"昆明", "25.0389", "102.7183"},
    {"沈阳", "41.8057", "123.4315"},
    {"哈尔滨", "45.8038", "126.5349"},
    {"乌鲁木齐", "43.8256", "87.6168"},
    {"海口", "20.0440", "110.1983"},
    {"香港", "22.3193", "114.1694"},
    {"台北", "25.0330", "121.5654"},
};

unsigned app_weather_city_count(void)
{
    return sizeof(cities) / sizeof(cities[0]);
}

const char *app_weather_city(unsigned city)
{
    return city < app_weather_city_count() ? cities[city].name : "";
}

const char *app_weather_condition(unsigned code)
{
    switch (code) {
        case 0: return "晴";
        case 1: case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: case 56: case 57: return "毛毛雨";
        case 61: case 63: case 65: case 66: case 67: return "雨";
        case 71: case 73: case 75: case 77: case 85: case 86: return "雪";
        case 80: case 81: case 82: return "阵雨";
        case 95: case 96: case 99: return "雷雨";
        default: return "—";
    }
}

bool app_weather_forecast_url(unsigned city, char *url, size_t capacity)
{
    if (city >= app_weather_city_count()) return false;
    const int length = snprintf(url, capacity,
        "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
        "&current=temperature_2m,relative_humidity_2m,weather_code"
        "&daily=temperature_2m_min,temperature_2m_max,weather_code"
        "&timezone=Asia%%2FShanghai&forecast_days=3&models=best_match",
        cities[city].latitude, cities[city].longitude);
    return length > 0 && (size_t)length < capacity;
}

static const char *whitespace(const char *cursor)
{
    while (cursor && isspace((unsigned char)*cursor)) ++cursor;
    return cursor;
}

static const char *skip_string(const char *cursor)
{
    for (++cursor; *cursor; ++cursor) {
        if (*cursor == '"') return cursor + 1;
        if (*cursor == '\\' && *++cursor == '\0') break;
    }
    return NULL;
}

static const char *skip_number(const char *cursor)
{
    if (*cursor == '-') ++cursor;
    if (*cursor == '0') ++cursor;
    else {
        if (!isdigit((unsigned char)*cursor)) return NULL;
        while (isdigit((unsigned char)*++cursor)) {}
    }
    if (*cursor == '.') {
        if (!isdigit((unsigned char)*++cursor)) return NULL;
        while (isdigit((unsigned char)*++cursor)) {}
    }
    if (*cursor == 'e' || *cursor == 'E') {
        ++cursor;
        if (*cursor == '+' || *cursor == '-') ++cursor;
        if (!isdigit((unsigned char)*cursor)) return NULL;
        while (isdigit((unsigned char)*++cursor)) {}
    }
    return cursor;
}

/* The backing buffer is bounded and NUL terminated by the HTTP client. Only
 * the requested fields are decoded; other members are skipped with a depth cap. */
static const char *skip_value(const char *cursor, unsigned depth)
{
    cursor = whitespace(cursor);
    if (!cursor || !*cursor || depth > 6) return NULL;
    if (*cursor == '"') return skip_string(cursor);
    if (*cursor == '{' || *cursor == '[') {
        const bool object = *cursor == '{';
        const char close = object ? '}' : ']';
        cursor = whitespace(cursor + 1);
        if (*cursor == close) return cursor + 1;
        for (;;) {
            if (object) {
                if (*cursor != '"') return NULL;
                cursor = whitespace(skip_string(cursor));
                if (!cursor || *cursor++ != ':') return NULL;
            }
            cursor = whitespace(skip_value(cursor, depth + 1));
            if (!cursor) return NULL;
            if (*cursor == close) return cursor + 1;
            if (*cursor != ',') return NULL;
            cursor = whitespace(cursor + 1);
        }
    }
    if (strncmp(cursor, "true", 4) == 0 || strncmp(cursor, "null", 4) == 0) return cursor + 4;
    if (strncmp(cursor, "false", 5) == 0) return cursor + 5;
    return skip_number(cursor);
}

static const char *member(const char *object, const char *key)
{
    object = whitespace(object);
    if (!object || *object != '{') return NULL;
    const char *cursor = whitespace(object + 1);
    const size_t key_length = strlen(key);
    while (*cursor == '"') {
        const char *name = cursor + 1;
        const char *after_name = skip_string(cursor);
        if (!after_name) break;
        cursor = whitespace(after_name);
        if (*cursor != ':') break;
        cursor = whitespace(cursor + 1);
        if ((size_t)(after_name - name - 1) == key_length && memcmp(name, key, key_length) == 0)
            return cursor;
        cursor = whitespace(skip_value(cursor, 0));
        if (!cursor || *cursor != ',') break;
        cursor = whitespace(cursor + 1);
    }
    return NULL;
}

static const char *read_number(const char *cursor, int minimum, int maximum, int *out)
{
    if (!cursor || (*cursor != '-' && !isdigit((unsigned char)*cursor))) return NULL;
    char *end;
    const float value = strtof(cursor, &end);
    if (end == cursor || !isfinite(value) || value < minimum || value > maximum)
        return NULL;
    const char *next = whitespace(end);
    if (*next != ',' && *next != '}' && *next != ']') return NULL;
    *out = (int)lroundf(value);
    return next;
}

static unsigned two_digits(const char *digits)
{
    return (unsigned)(digits[0] - '0') * 10U + (unsigned)(digits[1] - '0');
}

static const char *read_date(const char *cursor, bool with_time, char *out)
{
    static const char format[] = "0000-00-00T00:00";
    if (!cursor || *cursor++ != '"') return NULL;
    const size_t length = with_time ? sizeof(format) - 1 : 10;
    for (size_t index = 0; index < length; ++index) {
        if (format[index] == '0' ? !isdigit((unsigned char)cursor[index]) :
                                   cursor[index] != format[index]) return NULL;
    }
    if (cursor[length] != '"') return NULL;
    const unsigned year = two_digits(cursor) * 100 + two_digits(cursor + 2);
    if (!app_calendar_valid(year, two_digits(cursor + 5), two_digits(cursor + 8),
                            with_time ? two_digits(cursor + 11) : 0,
                            with_time ? two_digits(cursor + 14) : 0, 0)) return NULL;
    memcpy(out, cursor, length);
    out[length] = '\0';
    return whitespace(cursor + length + 1);
}

static bool number_array(const char *cursor, int minimum, int maximum, int out[APP_WEATHER_DAYS])
{
    if (!cursor || *cursor != '[') return false;
    for (size_t index = 0; index < APP_WEATHER_DAYS; ++index) {
        cursor = read_number(whitespace(cursor + 1), minimum, maximum, &out[index]);
        if (!cursor || *cursor != (index + 1 < APP_WEATHER_DAYS ? ',' : ']')) return false;
    }
    return true;
}

bool app_weather_parse(const char *json, app_weather_snapshot_t *out)
{
    const char *end = whitespace(skip_value(json, 0));
    if (!end || *end) return false;
    const char *current = member(json, "current");
    const char *daily = member(json, "daily");
    app_weather_snapshot_t parsed = {.valid = true, .city = out->city};
    int humidity, weather_code;
    int minimum[APP_WEATHER_DAYS], maximum[APP_WEATHER_DAYS], codes[APP_WEATHER_DAYS];
    if (!read_number(member(current, "temperature_2m"), -100, 100, &parsed.temperature_c) ||
        !read_number(member(current, "relative_humidity_2m"), 0, 100, &humidity) ||
        !read_number(member(current, "weather_code"), 0, 99, &weather_code) ||
        !read_date(member(current, "time"), true, parsed.observed) ||
        !number_array(member(daily, "temperature_2m_min"), -100, 100, minimum) ||
        !number_array(member(daily, "temperature_2m_max"), -100, 100, maximum) ||
        !number_array(member(daily, "weather_code"), 0, 99, codes)) return false;
    const char *dates = member(daily, "time");
    if (!dates || *dates != '[') return false;
    for (size_t index = 0; index < APP_WEATHER_DAYS; ++index) {
        app_weather_day_t *day = &parsed.daily[index];
        dates = read_date(whitespace(dates + 1), false, day->date);
        if (!dates || *dates != (index + 1 < APP_WEATHER_DAYS ? ',' : ']') ||
            minimum[index] > maximum[index]) return false;
        day->min_c = minimum[index];
        day->max_c = maximum[index];
        day->weather_code = (unsigned)codes[index];
    }
    parsed.humidity = (unsigned)humidity;
    parsed.weather_code = (unsigned)weather_code;
    *out = parsed;
    return true;
}
