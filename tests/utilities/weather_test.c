#include "app_weather_data.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char forecast[] =
    "{\"latitude\":31.2,\"longitude\":121.5,\"current_units\":{\"temperature_2m\":\"°C\"},"
    "\"current\": {\"time\":\"2028-02-29T08:15\",\"interval\":900,\"temperature_2m\":-2.6,"
    "\"relative_humidity_2m\":75,\"weather_code\":61},"
    "\"daily\":{\"time\":[\"2028-02-29\",\"2028-03-01\",\"2028-03-02\"],"
    "\"temperature_2m_min\":[-4,-6,5],\"temperature_2m_max\":[2,4,12],\"weather_code\":[61,73,1]}}";

static void replace_once(char *body, const char *needle, const char *replacement)
{
    char *match = strstr(body, needle);
    const size_t length = strlen(needle);
    assert(match && length == strlen(replacement));
    memcpy(match, replacement, length);
}

int main(int argc, char **argv)
{
    app_weather_snapshot_t weather = {.city = 1};
    assert(app_weather_parse(forecast, &weather));
    assert(weather.valid && weather.city == 1 && weather.temperature_c == -3);
    assert(weather.humidity == 75 && weather.weather_code == 61);
    assert(strcmp(weather.observed, "2028-02-29T08:15") == 0);
    assert(strcmp(weather.daily[2].date, "2028-03-02") == 0);
    assert(weather.daily[1].min_c == -6 && weather.daily[1].weather_code == 73);
    const app_weather_snapshot_t expected = weather;
    char invalid[sizeof(forecast)];
    for (size_t length = 0; length < strlen(forecast); ++length) {
        memcpy(invalid, forecast, length);
        invalid[length] = '\0';
        assert(!app_weather_parse(invalid, &weather));
    }
    memcpy(invalid, forecast, sizeof(forecast));
    replace_once(invalid, "2028-02-29T08:15", "2027-02-29T08:15");
    assert(!app_weather_parse(invalid, &weather));
    memcpy(invalid, forecast, sizeof(forecast));
    replace_once(invalid, "\"relative_humidity_2m\":75", "\"relative_humidity_2m\":-1");
    assert(!app_weather_parse(invalid, &weather));
    memcpy(invalid, forecast, sizeof(forecast));
    replace_once(invalid, "[-4,-6,5]", "[-4,-6,9]");
    replace_once(invalid, "[2,4,12]", "[2,4,02]");
    assert(!app_weather_parse(invalid, &weather));
    assert(weather.temperature_c == expected.temperature_c);
    assert(strcmp(weather.observed, expected.observed) == 0);

    char url[320];
    assert(app_weather_forecast_url(0, url, sizeof(url)));
    assert(strstr(url, "latitude=31.2304&longitude=121.4737"));
    assert(strstr(url, "timezone=Asia%2FShanghai&forecast_days=3&models=best_match"));
    assert(!app_weather_forecast_url(app_weather_city_count(), url, sizeof(url)));
    assert(!app_weather_forecast_url(0, url, 16));
    assert(strcmp(app_weather_city(0), "上海") == 0);
    assert(strcmp(app_weather_condition(95), "雷雨") == 0);
    if (argc == 2) {
        FILE *sample = fopen(argv[1], "rb");
        assert(sample);
        char response[4096];
        const size_t count = fread(response, 1, sizeof(response) - 1, sample);
        assert(count && feof(sample) && !ferror(sample));
        fclose(sample);
        response[count] = '\0';
        assert(app_weather_parse(response, &weather));
        puts("Live Shanghai forecast response parsed by production code");
    }
    puts("Open-Meteo forecast payload, truncation, ranges, dates and location checks passed");
    return 0;
}
