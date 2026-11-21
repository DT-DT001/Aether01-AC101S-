#include "gnss_nmea.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const RMC = "GNRMC,120000.000,A,2230.0000,N,11330.0000,E,10.0,90.0,211126,,,A";
static const char *const GGA = "GNGGA,120000.000,2230.0000,N,11330.0000,E,1,08,0.9,12.3,M,0,M,,";

static size_t wire_sentence(const char *body, char *wire, size_t capacity)
{
    unsigned checksum = 0;
    for (const unsigned char *p = (const unsigned char *)body; *p; ++p) checksum ^= *p;
    int n = snprintf(wire, capacity, "$%s*%02X\r\n", body, checksum);
    assert(n > 0 && (size_t)n < capacity);
    return (size_t)n;
}

static void feed(gnss_nmea_parser_t *p, const char *body, uint64_t now)
{
    char wire[512];
    size_t n = wire_sentence(body, wire, sizeof(wire));
    gnss_nmea_feed(p, wire, n, now);
}

static gnss_nmea_snapshot_t snapshot(gnss_nmea_parser_t *p, uint64_t now)
{
    gnss_nmea_snapshot_t result;
    gnss_nmea_get_snapshot(p, now, &result);
    return result;
}

static const gnss_satellite_t *find_sat(const gnss_nmea_snapshot_t *s, gnss_system_t system, unsigned prn)
{
    for (unsigned i = 0; i < s->satellite_count; ++i)
        if (s->satellites[i].system == system && s->satellites[i].prn == prn) return &s->satellites[i];
    return NULL;
}

static void feed_gsa(gnss_nmea_parser_t *p, const char *talker, unsigned system, unsigned dimension,
                     unsigned prn, uint64_t now)
{
    char body[200];
    int n = snprintf(body, sizeof(body), "%sGSA,A,%u", talker, dimension);
    assert(n > 0);
    size_t at = (size_t)n;
    for (unsigned i = 0; i < 12; ++i) {
        n = snprintf(body + at, sizeof(body) - at, i == 0 && prn ? ",%02u" : ",", prn);
        assert(n > 0); at += (size_t)n;
    }
    n = snprintf(body + at, sizeof(body) - at, ",1.2,0.8,1.0,%u", system);
    assert(n > 0 && (size_t)n < sizeof(body) - at);
    feed(p, body, now);
}

static void test_basics(gnss_nmea_parser_t *p)
{
    gnss_nmea_reset(p);
    feed(p, RMC, 0); feed(p, GGA, 0);
    gnss_nmea_snapshot_t s = snapshot(p, 0);
    assert(s.accepted_sentences == 2 && s.position_valid && s.time_valid && s.date_valid);
    assert(s.last_time_ms == 0 && s.last_fix_ms == 0);
    assert(fabs(s.latitude_deg - 22.5) < 1e-8 && fabs(s.longitude_deg - 113.5) < 1e-8);
    assert(s.year == 2026 && s.month == 11 && s.day == 21 && s.hour == 12);
    assert(s.satellites_used == 8 && s.fix_quality == 1);
    assert(s.altitude_valid && fabsf(s.altitude_m - 12.3f) < 0.001f);
    assert(s.speed_valid && fabsf(s.speed_kmh - 18.52f) < 0.001f);
    assert(s.course_valid && s.course_deg == 90 && s.hdop_valid);
    s = snapshot(p, 5000); assert(s.position_valid && s.time_valid);
    s = snapshot(p, 5001);
    assert(!s.position_valid && !s.time_valid && !s.date_valid && !s.altitude_valid && !s.speed_valid);
    assert(!s.course_valid && !s.hdop_valid && !s.satellites_used && !s.fix_quality);
    assert(s.last_fix_ms == 0);
    feed(p, RMC, 6000); feed(p, GGA, 6000);
    s = snapshot(p, 6000); assert(s.position_valid && s.last_fix_ms == 6000);
    s = snapshot(p, 500); assert(!s.position_valid && !s.time_valid);
}

static void test_stream(gnss_nmea_parser_t *p)
{
    gnss_nmea_reset(p);
    char wire[512]; size_t n = wire_sentence(RMC, wire, sizeof(wire));
    gnss_nmea_feed(p, "noise\r\n\001", 8, 0);
    for (size_t i = 0; i < n; ++i) gnss_nmea_feed(p, wire + i, 1, 10);
    gnss_nmea_snapshot_t s = snapshot(p, 10);
    assert(s.position_valid && s.accepted_sentences == 1);
    wire[n - 4] = wire[n - 4] == '0' ? '1' : '0';
    gnss_nmea_feed(p, wire, n, 11);
    s = snapshot(p, 11); assert(s.accepted_sentences == 1 && s.bad_checksums == 1);
    gnss_nmea_feed(p, "$GNRMC,broken", strlen("$GNRMC,broken"), 12);
    feed(p, GGA, 12);
    s = snapshot(p, 12); assert(s.accepted_sentences == 2 && s.malformed_sentences >= 1);
    char long_line[500]; memset(long_line, 'A', sizeof(long_line)); long_line[0] = '$';
    gnss_nmea_feed(p, long_line, sizeof(long_line), 13);
    feed(p, RMC, 13); s = snapshot(p, 13);
    assert(s.accepted_sentences == 3 && s.malformed_sentences >= 2);
    gnss_nmea_feed(p, "$GPGGA,1\n", strlen("$GPGGA,1\n"), 14);
    s = snapshot(p, 14); assert(s.accepted_sentences == 3 && s.malformed_sentences >= 3);
    gnss_nmea_feed(p, NULL, 0, 15);
    gnss_nmea_feed(NULL, wire, n, 15);
    gnss_nmea_get_snapshot(NULL, 15, &s); assert(!s.position_valid && !s.accepted_sentences);
    gnss_nmea_get_snapshot(p, 15, NULL);
    gnss_nmea_reset(NULL); gnss_nmea_free(NULL);
}

static void test_invalid_fields(gnss_nmea_parser_t *p)
{
    static const char *const bad[] = {
        "GNRMC,120000,A,,,,,1,2,211126,,,A",
        "GNRMC,120000,A,2260.000,N,11330.000,E,1,2,211126,,,A",
        "GNRMC,120000,A,9100.000,N,11330.000,E,1,2,211126,,,A",
        "GNRMC,120000,A,2230.000,N,18000.001,E,1,2,211126,,,A",
        "GNRMC,120000,A,2230.000,X,11330.000,E,1,2,211126,,,A",
        "GNRMC,240000,A,2230.000,N,11330.000,E,1,2,211126,,,A",
        "GNRMC,120060,A,2230.000,N,11330.000,E,1,2,211126,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,NaN,2,211126,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,1,Inf,211126,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,1e2,2,211126,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,-1,2,211126,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,1,361,211126,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,1,2,290223,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,1,2,310426,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,1,2,001126,,,A",
        "GNRMC,120000,A,2230.000,N,11330.000,E,1,2,210026,,,A",
        "GNGGA,120000,2230,N,11330,E,1,08,NaN,12,M,,,",
        "GNGGA,120000,2230,N,11330,E,1,08,1,Inf,M,,,",
        "GNGGA,120000,2230,N,11330,E,1,08,1,12,F,,,",
        "GNGGA,120000,2230,N,11330,E,9,08,1,12,M,,,",
        "GPGSV,1,1,01,01,91,100,30",
        "GPGSV,1,1,01,01,10,360,30",
        "GPGSV,1,1,01,01,10,100,NaN",
        "GPGSV,1,1,01,01,10,100,100",
        "GPGSV,0,1,01,01,10,100,30",
        "GPGSV,1,0,01,01,10,100,30",
        "GPGSV,2,1,01,01,10,100,30",
        "GPGSV,1,1,01,01,10,100,30,1,2",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        gnss_nmea_reset(p); feed(p, bad[i], 1);
        gnss_nmea_snapshot_t s = snapshot(p, 1);
        if (s.malformed_sentences != 1) fprintf(stderr, "accepted invalid case %zu: %s\n", i, bad[i]);
        assert(s.malformed_sentences == 1 && !s.position_valid && !s.time_valid && !s.satellite_count);
    }
}

static void test_boundaries_and_loss(gnss_nmea_parser_t *p)
{
    gnss_nmea_reset(p);
    feed(p, "GPRMC,000000.000,A,0000.0000,S,00000.0000,W,0,0,290224,,,A", 1);
    gnss_nmea_snapshot_t s = snapshot(p, 1);
    assert(s.position_valid && s.latitude_deg == 0 && s.longitude_deg == 0 && s.speed_valid && s.course_valid);
    assert(s.date_valid && s.day == 29 && s.month == 2);
    feed(p, "GPRMC,235959,A,9000.0000,S,18000.0000,W,0,360,311299,,,A", 2);
    s = snapshot(p, 2);
    assert(s.position_valid && s.latitude_deg == -90 && s.longitude_deg == -180 && s.course_deg == 0);
    assert(s.year == 1999 && s.hour == 23 && s.minute == 59 && s.second == 59);
    feed(p, "GPRMC,120000,A,2230.0000,S,11330.0000,W,,,211126,,,A", 3);
    s = snapshot(p, 3);
    assert(s.position_valid && s.latitude_deg == -22.5 && s.longitude_deg == -113.5);
    assert(!s.speed_valid && !s.course_valid);
    feed(p, "GNGGA,120000,,,,,0,00,,,,,,,", 4);
    s = snapshot(p, 4); assert(!s.position_valid && !s.satellites_used && !s.altitude_valid);
    feed(p, RMC, 5); s = snapshot(p, 5); assert(!s.position_valid);
    feed(p, GGA, 6); s = snapshot(p, 6); assert(s.position_valid);
    feed(p, "GNRMC,120001,V,,,,,,,211126,,,N", 7);
    s = snapshot(p, 7); assert(!s.position_valid && !s.time_valid && !s.date_valid);
    feed(p, GGA, 8); s = snapshot(p, 8); assert(!s.position_valid);
    feed(p, RMC, 9); s = snapshot(p, 9); assert(s.position_valid);
    gnss_nmea_reset(p); feed(p, GGA, UINT64_MAX - 2U);
    s = snapshot(p, UINT64_MAX); assert(s.position_valid);
}

static void test_gsv_cycles(gnss_nmea_parser_t *p)
{
    gnss_nmea_reset(p);
    feed(p, "GPGSV,2,1,05,01,10,000,30,02,20,090,31,03,30,180,32,04,40,270,", 0);
    gnss_nmea_snapshot_t s = snapshot(p, 0); assert(!s.satellite_count);
    feed(p, "GPGSV,2,2,05,05,90,359,45", 1);
    s = snapshot(p, 1); assert(s.satellite_count == 5 && s.last_satellite_ms == 1);
    feed(p, "GPGSV,2,2,05,99,30,100,99", 1);
    s = snapshot(p, 1);
    assert(s.satellite_count == 5 && find_sat(&s, GNSS_SYSTEM_GPS, 5));
    assert(!find_sat(&s, GNSS_SYSTEM_GPS, 99));
    const gnss_satellite_t *sat = find_sat(&s, GNSS_SYSTEM_GPS, 4);
    assert(sat && !sat->signal_valid && sat->geometry_valid);
    feed(p, "GPGSV,1,1,01,01,15,100,50,2", 2);
    s = snapshot(p, 2); assert(s.satellite_count == 5);
    sat = find_sat(&s, GNSS_SYSTEM_GPS, 1); assert(sat && sat->signal_valid && sat->snr_dbhz == 50);
    feed(p, "BDGSV,1,1,01,01,20,090,40", 2);
    s = snapshot(p, 2); assert(s.satellite_count == 6 && find_sat(&s, GNSS_SYSTEM_BEIDOU, 1));
    feed(p, "GPGSV,2,1,05,10,10,000,30,11,20,090,31,12,30,180,32,13,40,270,", 3);
    s = snapshot(p, 3); assert(s.satellite_count == 6 && find_sat(&s, GNSS_SYSTEM_GPS, 5));
    feed(p, "GPGSV,2,2,05,14,90,359,45", 4);
    s = snapshot(p, 4);
    assert(s.satellite_count == 7 && !find_sat(&s, GNSS_SYSTEM_GPS, 5));
    /* The other signal retains satellite 01 until that signal ages out. */
    assert(find_sat(&s, GNSS_SYSTEM_GPS, 1));
    s = snapshot(p, 15003); assert(s.satellite_count == 5);
    s = snapshot(p, 15005); assert(!s.satellite_count);
    feed(p, "GBGSV,1,1,01,12,,,", 16000);
    s = snapshot(p, 16000); sat = find_sat(&s, GNSS_SYSTEM_BEIDOU, 12);
    assert(sat && !sat->signal_valid && !sat->geometry_valid);
    feed(p, "GBGSV,1,1,00,", 16001); s = snapshot(p, 16001); assert(!s.satellite_count);
}

static void test_used_systems(gnss_nmea_parser_t *p)
{
    gnss_nmea_reset(p);
    feed(p, "GPGSV,1,1,01,01,20,090,40", 0);
    feed(p, "BDGSV,1,1,01,01,20,090,35", 0);
    feed_gsa(p, "GN", 1, 3, 1, 1);
    gnss_nmea_snapshot_t s = snapshot(p, 1);
    assert(find_sat(&s, GNSS_SYSTEM_GPS, 1)->used && !find_sat(&s, GNSS_SYSTEM_BEIDOU, 1)->used);
    feed_gsa(p, "GN", 4, 3, 1, 2); s = snapshot(p, 2);
    assert(find_sat(&s, GNSS_SYSTEM_GPS, 1)->used && find_sat(&s, GNSS_SYSTEM_BEIDOU, 1)->used);
    feed_gsa(p, "GN", 1, 1, 0, 3); s = snapshot(p, 3);
    assert(!find_sat(&s, GNSS_SYSTEM_GPS, 1)->used && find_sat(&s, GNSS_SYSTEM_BEIDOU, 1)->used);
    feed(p, RMC, 4); s = snapshot(p, 4);
    assert(s.satellites_used == 1 && s.fix_dimension == 3 && s.hdop_valid);
    feed(p, "GPGSV,1,1,01,01,20,090,40", 15004);
    feed(p, "BDGSV,1,1,01,01,20,090,35", 15004);
    s = snapshot(p, 15004);
    assert(!find_sat(&s, GNSS_SYSTEM_GPS, 1)->used && !find_sat(&s, GNSS_SYSTEM_BEIDOU, 1)->used);
}

static void test_padded_gsv_and_legacy_gsa(gnss_nmea_parser_t *p)
{
    gnss_nmea_reset(p);
    feed(p, "GPGSV,2,1,05,01,10,000,30,02,20,090,31,03,30,180,32,04,40,270,", 0);
    char padded[200] = "GPGSV,2,2,05,05,90,359,45";
    for (unsigned i = 0; i < 12; ++i) strcat(padded, ",");
    feed(p, padded, 1);
    gnss_nmea_snapshot_t s = snapshot(p, 1);
    assert(s.accepted_sentences == 2 && s.satellite_count == 5);
    assert(find_sat(&s, GNSS_SYSTEM_GPS, 5)->snr_dbhz == 45);
    /* Same padding with an explicit signalId must not be mistaken for PRN. */
    gnss_nmea_reset(p);
    strcpy(padded, "GPGSV,1,1,01,01,20,090,50");
    for (unsigned i = 0; i < 12; ++i) strcat(padded, ",");
    strcat(padded, ",2"); feed(p, padded, 2);
    s = snapshot(p, 2);
    assert(s.satellite_count == 1 && s.accepted_sentences == 1);
    assert(find_sat(&s, GNSS_SYSTEM_GPS, 1)->snr_dbhz == 50);
    /* A padded zero-satellite report clears only the matching signal stream. */
    strcpy(padded, "GPGSV,1,1,00");
    for (unsigned i = 0; i < 16; ++i) strcat(padded, ",");
    strcat(padded, ",2"); feed(p, padded, 3);
    s = snapshot(p, 3); assert(!s.satellite_count && s.accepted_sentences == 2);
    gnss_nmea_reset(p);
    feed(p, "GPGSV,1,1,01,,20,090,50,,,,,,,,,,,,", 4);
    s = snapshot(p, 4); assert(s.malformed_sentences == 1 && !s.satellite_count);
    feed(p, "GPGSV,1,1,01,01,20,090,50,02,,,,,,,,,,,", 5);
    s = snapshot(p, 5); assert(s.malformed_sentences == 2 && !s.satellite_count);
    /* No systemId: leave an ambiguous GNGSA PRN 01 UNKNOWN, not GPS or BD. */
    gnss_nmea_reset(p);
    feed(p, "GPGSV,1,1,01,01,20,090,40", 0);
    feed(p, "BDGSV,1,1,01,01,20,090,35", 0);
    strcpy(padded, "GNGSA,A,3,01");
    for (unsigned i = 1; i < 12; ++i) strcat(padded, ",");
    strcat(padded, ",1.2,0.8,1.0"); feed(p, padded, 1);
    s = snapshot(p, 1);
    assert(s.malformed_sentences == 0);
    assert(!find_sat(&s, GNSS_SYSTEM_GPS, 1)->used && !find_sat(&s, GNSS_SYSTEM_BEIDOU, 1)->used);
    /* The identical legacy sentence with GP talker is unambiguous. */
    padded[1] = 'P'; feed(p, padded, 2); s = snapshot(p, 2);
    assert(find_sat(&s, GNSS_SYSTEM_GPS, 1)->used && !find_sat(&s, GNSS_SYSTEM_BEIDOU, 1)->used);
}

static void test_capacity_and_noise(gnss_nmea_parser_t *p)
{
    gnss_nmea_reset(p);
    for (unsigned page = 1; page <= 32; ++page) {
        unsigned prn = (page - 1U) * 4U + 1U;
        char body[180];
        int n = snprintf(body, sizeof(body), "GPGSV,32,%u,128,%u,10,100,20,%u,20,200,30,%u,30,300,40,%u,40,000,50",
                         page, prn, prn + 1U, prn + 2U, prn + 3U);
        assert(n > 0 && (size_t)n < sizeof(body)); feed(p, body, page);
    }
    gnss_nmea_snapshot_t s = snapshot(p, 32);
    assert(s.satellite_count == GNSS_MAX_SATELLITES && find_sat(&s, GNSS_SYSTEM_GPS, 48));
    assert(!find_sat(&s, GNSS_SYSTEM_GPS, 49));
    srand(1234);
    for (unsigned i = 0; i < 20000; ++i) {
        unsigned char bytes[19];
        for (size_t j = 0; j < sizeof(bytes); ++j) bytes[j] = (unsigned char)(rand() & 255);
        gnss_nmea_feed(p, (const char *)bytes, sizeof(bytes), i);
        s = snapshot(p, i); assert(s.satellite_count <= GNSS_MAX_SATELLITES);
    }
    feed(p, RMC, 30000); feed(p, GGA, 30000);
    s = snapshot(p, 30000); assert(s.position_valid);
}

int main(void)
{
    gnss_nmea_parser_t *p = gnss_nmea_new(); assert(p);
    test_basics(p); test_stream(p); test_invalid_fields(p); test_boundaries_and_loss(p);
    test_gsv_cycles(p); test_used_systems(p); test_padded_gsv_and_legacy_gsa(p); test_capacity_and_noise(p);
    gnss_nmea_free(p);
    puts("GNSS NMEA production parser: all regression groups passed");
    return 0;
}
