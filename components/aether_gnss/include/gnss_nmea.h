#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_MAX_SATELLITES 48
#define GNSS_NMEA_FIX_TIMEOUT_MS UINT64_C(5000)
#define GNSS_NMEA_SATELLITE_TIMEOUT_MS UINT64_C(15000)

typedef enum {
    GNSS_SYSTEM_UNKNOWN = 0,
    GNSS_SYSTEM_GPS,
    GNSS_SYSTEM_GLONASS,
    GNSS_SYSTEM_BEIDOU,
    GNSS_SYSTEM_GALILEO,
    GNSS_SYSTEM_QZSS,
    GNSS_SYSTEM_SBAS,
} gnss_system_t;

typedef struct {
    gnss_system_t system;
    uint16_t prn;
    uint16_t azimuth_deg;
    uint8_t elevation_deg;
    uint8_t snr_dbhz;
    bool signal_valid;
    /* From a constellation-specific GSA or its NMEA 4.1 systemId. Legacy
     * GNGSA without systemId cannot safely distinguish overlapping PRNs. */
    bool used;
    /* A missing angle must not become a fictitious point on the horizon. */
    bool geometry_valid;
} gnss_satellite_t;

typedef struct {
    bool position_valid, time_valid, date_valid, altitude_valid;
    bool speed_valid, course_valid, hdop_valid;
    double latitude_deg, longitude_deg;
    float altitude_m, speed_kmh, course_deg, hdop;
    uint8_t fix_quality, fix_dimension, satellites_used, satellite_count;
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    gnss_satellite_t satellites[GNSS_MAX_SATELLITES];
    uint32_t accepted_sentences, bad_checksums, malformed_sentences;
    uint64_t last_sentence_ms, last_fix_ms, last_time_ms, last_satellite_ms;
} gnss_nmea_snapshot_t;

typedef struct gnss_nmea_parser gnss_nmea_parser_t;

/* Portable, allocation only in new/free, and deliberately not thread-safe.
 * The owner serializes feed/get_snapshot/reset with its service mutex.
 * now_ms is a monotonic clock, and zero is a valid initial timestamp. */
gnss_nmea_parser_t *gnss_nmea_new(void);
void gnss_nmea_free(gnss_nmea_parser_t *parser);
void gnss_nmea_reset(gnss_nmea_parser_t *parser);
void gnss_nmea_feed(gnss_nmea_parser_t *parser, const char *data, size_t len, uint64_t now_ms);
void gnss_nmea_get_snapshot(gnss_nmea_parser_t *parser, uint64_t now_ms,
                            gnss_nmea_snapshot_t *out);

#ifdef __cplusplus
}
#endif
