#include "app_weather_data.h"

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESP_ERR_HTTP_CONNECT 0x7002
#define ESP_ERR_HTTP_FETCH_HEADER 0x7004
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define HTTP_ADDR_TYPE_INET 1

typedef struct {
    const char *url;
    void (*crt_bundle_attach)(void);
    int addr_type, timeout_ms, buffer_size, buffer_size_tx;
    bool disable_auto_redirect;
} esp_http_client_config_t;

typedef struct {
    const char *body;
    int size, status, stalls, timeout_ms, cursor;
    int64_t header_length;
    esp_err_t open_error;
    bool complete;
} exchange_t;

typedef exchange_t *esp_http_client_handle_t;
static exchange_t exchanges[2];
static unsigned initialized, closed, body_reads, tasks_deleted;
static int64_t clock_us;
static bool online, powered;
static int s_lock;
static app_weather_snapshot_t s_weather;
static bool s_weather_running;
static int64_t s_weather_due;
static char last_warning[256];

#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_LOGW(tag, ...) record_warning(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ((void)(tag))

static void record_warning(const char *tag, const char *format, ...)
{
    assert(strcmp(tag, "weather") == 0);
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_warning, sizeof(last_warning), format, arguments);
    va_end(arguments);
}

const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "error"; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool wifi_connected(void) { return online; }
static bool power_running(void) { return powered; }
static void vTaskDelete(void *task) { assert(!task); ++tasks_deleted; }
static void esp_crt_bundle_attach(void) {}

static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(initialized < 2);
    assert(strstr(config->url, "https://api.open-meteo.com/v1/forecast?"));
    assert(config->addr_type == HTTP_ADDR_TYPE_INET && config->timeout_ms == 8000);
    assert(config->disable_auto_redirect && config->crt_bundle_attach == esp_crt_bundle_attach);
    return &exchanges[initialized++];
}

static esp_err_t esp_http_client_set_header(exchange_t *client, const char *name, const char *value)
{
    assert(client && (strcmp(name, "Accept") == 0 ? strcmp(value, "application/json") == 0 :
                     strcmp(name, "Accept-Encoding") == 0 && strcmp(value, "identity") == 0));
    return ESP_OK;
}

static esp_err_t esp_http_client_open(exchange_t *client, int length)
{
    assert(length == 0);
    return client->open_error;
}

static int64_t esp_http_client_fetch_headers(exchange_t *client) { return client->header_length; }
static int esp_http_client_get_status_code(exchange_t *client) { return client->status; }
static bool esp_http_client_is_complete_data_received(exchange_t *client)
{
    return client->complete && client->cursor == client->size;
}

static void esp_http_client_set_timeout_ms(exchange_t *client, int timeout)
{
    assert(timeout >= 1 && timeout <= 1500);
    client->timeout_ms = timeout;
}

static int esp_http_client_read(exchange_t *client, char *output, int count)
{
    assert(count >= 1 && count <= 128 && client->timeout_ms);
    ++body_reads;
    if (client->stalls > 0) {
        --client->stalls;
        clock_us += (int64_t)client->timeout_ms * 1000;
        return -ESP_ERR_HTTP_EAGAIN;
    }
    const int remaining = client->size - client->cursor;
    if (count > remaining) count = remaining;
    if (count) memcpy(output, client->body + client->cursor, count);
    client->cursor += count;
    return count;
}

static int esp_http_client_get_errno(exchange_t *client) { return client->open_error ? 113 : 0; }
static esp_err_t esp_http_client_get_and_clear_last_tls_error(exchange_t *client, int *code, int *flags)
{
    *code = 0;
    *flags = 0;
    return client->open_error ? 0x8006 : ESP_OK;
}

static void esp_http_client_cleanup(exchange_t *client) { assert(client); ++closed; }

#include "weather_http.inc"

static const char forecast[] =
    "{\"current\":{\"time\":\"2028-02-29T08:15\",\"temperature_2m\":-2.6,"
    "\"relative_humidity_2m\":75,\"weather_code\":61},"
    "\"daily\":{\"time\":[\"2028-02-29\",\"2028-03-01\",\"2028-03-02\"],"
    "\"temperature_2m_min\":[-4,-6,5],\"temperature_2m_max\":[2,4,12],\"weather_code\":[61,73,1]}}";

static void reset(void)
{
    const exchange_t successful = {.body = forecast, .size = sizeof(forecast) - 1,
                                   .header_length = sizeof(forecast) - 1, .status = 200,
                                   .complete = true};
    exchanges[0] = exchanges[1] = successful;
    initialized = closed = body_reads = tasks_deleted = 0;
    clock_us = s_weather_due = 0;
    s_weather = (app_weather_snapshot_t){.city = 1, .fetching = true};
    s_weather_running = online = powered = true;
    last_warning[0] = '\0';
}

static void fetch(unsigned attempts)
{
    weather_task((void *)(uintptr_t)s_weather.city);
    assert(initialized == attempts && closed == attempts && tasks_deleted == 1);
    assert(!s_weather.fetching && !s_weather_running && s_weather_due > clock_us);
}

int main(void)
{
    reset();
    exchanges[0].open_error = ESP_ERR_HTTP_CONNECT;
    exchanges[1].header_length = 0; /* Transfer-Encoding: chunked. */
    exchanges[1].stalls = 2;
    fetch(2);
    assert(s_weather.valid && s_weather.temperature_c == -3 && s_weather.city == 1);
    assert(clock_us == 3000000 && body_reads >= 4);
    assert(strstr(last_warning, "stage=open") && strstr(last_warning, "socket_errno=113"));

    reset();
    exchanges[0].open_error = ESP_ERR_HTTP_CONNECT;
    online = false;
    fetch(1);
    assert(s_weather.error == ESP_ERR_HTTP_CONNECT);

    reset();
    exchanges[0].stalls = 10;
    fetch(1);
    assert(s_weather.error == ESP_ERR_TIMEOUT && clock_us == 5000000 && body_reads == 4);
    assert(strstr(last_warning, "stage=body") && strstr(last_warning, "status=200"));

    reset();
    exchanges[0].header_length = -ESP_ERR_HTTP_EAGAIN;
    fetch(1);
    assert(s_weather.error == ESP_ERR_TIMEOUT && body_reads == 0 && strstr(last_warning, "stage=headers"));

    reset();
    exchanges[0].status = 429;
    fetch(1);
    assert(s_weather.error == ESP_ERR_INVALID_RESPONSE && body_reads == 0 && strstr(last_warning, "status=429"));

    reset();
    exchanges[0].complete = false;
    fetch(1);
    assert(s_weather.error == ESP_ERR_INVALID_RESPONSE && !s_weather.valid && strstr(last_warning, "stage=body"));

    reset();
    exchanges[0] = (exchange_t){.body = "{}", .size = 2, .header_length = 2, .status = 200, .complete = true};
    fetch(1);
    assert(s_weather.error == ESP_ERR_INVALID_RESPONSE && strstr(last_warning, "stage=parse"));
    puts("PASS weather HTTP: encrypted IPv4, one connected retry, chunked stalls/deadline, header/status and parse diagnostics");
    return 0;
}
