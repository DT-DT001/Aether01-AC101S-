/* Read-only audit reproductions against the real production parser. These
 * characterize both supported CASIC streams and deliberate conservative
 * no-fix policy. They do not send anything to the physical receiver. */
#include "gnss_nmea.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void wire(gnss_nmea_parser_t *p, const char *line, uint64_t now)
{
    const size_t length = strlen(line);
    /* Exercise realistic small RX fragments, including split checksum/CRLF. */
    for (size_t at = 0; at < length;) {
        size_t chunk = at % 19U + 1U;
        if (chunk > length - at) chunk = length - at;
        gnss_nmea_feed(p, line + at, chunk, now);
        at += chunk;
    }
}

static void body(gnss_nmea_parser_t *p, const char *payload, uint64_t now)
{
    unsigned checksum = 0;
    for (const unsigned char *c = (const unsigned char *)payload; *c; ++c) checksum ^= *c;
    char line[220];
    int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", payload, checksum);
    assert(n > 0 && (size_t)n < sizeof(line));
    wire(p, line, now);
}

static gnss_nmea_snapshot_t status(gnss_nmea_parser_t *p, uint64_t now)
{
    gnss_nmea_snapshot_t s;
    gnss_nmea_get_snapshot(p, now, &s);
    return s;
}

static void documented_frames(gnss_nmea_parser_t *p)
{
    /* Literal CASIC V4.2.0.3 examples, retaining their published checksum. */
    static const char *const frames[] = {
        "$GPGGA,235316.000,2959.9925,S,12000.0090,E,1,06,1.21,62.77,M,0.00,M,,*7B\r\n",
        "$GPRMC,235316.000,A,2959.9925,S,12000.0090,E,0.009,75.020,020711,,,A*45\r\n",
        "$GPGSA,A,3,05,21,31,12,18,29,,,,,,,2.56,1.21,2.25*01\r\n",
        "$GPGSV,3,1,10,25,68,053,47,21,59,306,49,29,56,161,49,31,36,265,49*79\r\n",
        "$GPGSV,3,2,10,12,29,048,49,05,22,123,49,18,13,000,49,01,00,000,49*72\r\n",
        "$GPGSV,3,3,10,14,00,000,03,16,00,000,27*7C\r\n",
    };
    gnss_nmea_reset(p);
    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); ++i) wire(p, frames[i], i * 40U);
    gnss_nmea_snapshot_t s = status(p, 240);
    assert(s.accepted_sentences == 6 && !s.bad_checksums && !s.malformed_sentences);
    assert(s.position_valid && s.time_valid && s.date_valid && s.altitude_valid && s.fix_dimension == 3);
    assert(s.year == 2011 && s.month == 7 && s.day == 2);
    assert(fabs(s.latitude_deg + 29.999875) < 1e-8 && fabs(s.longitude_deg - 120.00015) < 1e-8);
    assert(s.satellite_count == 10 && s.satellites_used == 6);
    unsigned used = 0;
    for (unsigned i = 0; i < s.satellite_count; ++i) used += s.satellites[i].used;
    assert(used == 6);
    puts("PASS literal published CASIC GGA/RMC/GSA/GSV checksums: 6 accepted, 10 visible, 6 used, valid fix/date");
}

static void make_epoch(char *rmc, size_t rcap, char *gga, size_t gcap, unsigned second,
                       bool valid, const char *talker)
{
    unsigned minute = second / 60U;
    second %= 60U;
    if (valid) {
        snprintf(rmc, rcap, "%sRMC,12%02u%02u.000,A,3112.00000,N,12130.00000,E,0.000,0.0,211126,,,A,V",
                 talker, minute, second);
        snprintf(gga, gcap, "%sGGA,12%02u%02u.000,3112.00000,N,12130.00000,E,1,08,1.2,12.3,M,4.5,M,,",
                 talker, minute, second);
    } else {
        snprintf(rmc, rcap, "%sRMC,12%02u%02u.000,V,,,,,,,211126,,,N,V", talker, minute, second);
        snprintf(gga, gcap, "%sGGA,12%02u%02u.000,,,,,0,00,99.99,,,,,,", talker, minute, second);
    }
}

static void coherent_one_hz(gnss_nmea_parser_t *p, bool rmc_first)
{
    char rmc[180], gga[180];
    gnss_nmea_reset(p);
    make_epoch(rmc, sizeof(rmc), gga, sizeof(gga), 0, false, "GN");
    body(p, rmc, 0); body(p, gga, 100);
    assert(!status(p, 100).position_valid);
    for (unsigned second = 1; second <= 1800; ++second) {
        uint64_t now = (uint64_t)second * 1000;
        make_epoch(rmc, sizeof(rmc), gga, sizeof(gga), second, true, "GN");
        body(p, rmc_first ? rmc : gga, now);
        body(p, rmc_first ? gga : rmc, now + 150);
        if (second % 2U == 0) {
            body(p, "GNGSA,A,3,05,21,31,12,18,29,,,,,,,2.56,1.21,2.25,1", now + 200);
            body(p, "GNGSA,A,3,01,02,03,04,,,,,,,,,2.56,1.21,2.25,4", now + 220);
            body(p, "GPGSV,1,1,02,05,29,048,49,21,59,306,49,1", now + 240);
            body(p, "BDGSV,1,1,02,01,30,060,35,02,60,120,40,1", now + 260);
        }
        gnss_nmea_snapshot_t s = status(p, now + 300);
        assert(s.position_valid && s.time_valid && s.date_valid && s.altitude_valid);
        assert(!s.bad_checksums && !s.malformed_sentences);
        assert(fabs(s.latitude_deg - 31.2) < 1e-8 && fabs(s.longitude_deg - 121.5) < 1e-8);
        if (second > 1) assert(s.satellite_count == 4 && s.fix_dimension == 3);
    }
    printf("PASS 1800 coherent CASIC 1Hz epochs, %s first: fix remains valid after both current sentences\n",
           rmc_first ? "RMC" : "GGA");
}

static void independent_sources(gnss_nmea_parser_t *p)
{
    char rmc[180], gga[180];
    for (unsigned source = 0; source < 2; ++source) {
        gnss_nmea_reset(p);
        for (unsigned second = 0; second < 120; ++second) {
            uint64_t now = (uint64_t)second * 1000;
            make_epoch(rmc, sizeof(rmc), gga, sizeof(gga), second, true, "GN");
            body(p, source == 0 ? rmc : gga, now);
            gnss_nmea_snapshot_t s = status(p, now);
            assert(s.position_valid && !s.bad_checksums && !s.malformed_sentences);
        }
    }
    puts("PASS RMC-only and GGA-only 120-second streams each independently acquire and retain position");
}

static void contradictory_no_fix_policy(gnss_nmea_parser_t *p)
{
    char good_rmc[180], good_gga[180], bad_rmc[180], bad_gga[180];
    gnss_nmea_reset(p);
    for (unsigned second = 0; second < 30; ++second) {
        uint64_t now = (uint64_t)second * 1000;
        make_epoch(good_rmc, sizeof(good_rmc), good_gga, sizeof(good_gga), second, true, "GN");
        make_epoch(bad_rmc, sizeof(bad_rmc), bad_gga, sizeof(bad_gga), second, false, "GN");
        body(p, good_rmc, now); body(p, bad_gga, now + 100);
        gnss_nmea_snapshot_t s = status(p, now + 200);
        assert(!s.position_valid && s.time_valid && s.accepted_sentences == (second + 1U) * 2U);
        assert(!s.bad_checksums && !s.malformed_sentences);
    }
    body(p, good_gga, 29300); assert(status(p, 29300).position_valid);
    puts("PASS characterized policy: repeated RMC A plus GGA quality0 vetoes fix indefinitely; consistent GGA immediately restores fix");
    /* The implementation compares receive freshness, not NMEA UTC epochs.
     * An old-but-newly-received no-fix therefore has the same veto effect. */
    gnss_nmea_reset(p);
    for (unsigned second = 0; second < 15; ++second) {
        uint64_t now = (uint64_t)second * 1000;
        make_epoch(good_rmc, sizeof(good_rmc), good_gga, sizeof(good_gga), second, true, "GN");
        body(p, good_rmc, now);
        body(p, "GNGGA,115959.000,,,,,0,00,99.99,,,,,,", now + 100);
        assert(!status(p, now + 200).position_valid);
    }
    puts("PASS characterized limitation: repeated stale-epoch no-fix also vetoes current RMC because freshness uses arrival time");
}

int main(void)
{
    gnss_nmea_parser_t *p = gnss_nmea_new(); assert(p);
    documented_frames(p); coherent_one_hz(p, true); coherent_one_hz(p, false);
    independent_sources(p); contradictory_no_fix_policy(p);
    gnss_nmea_free(p);
    puts("PASS CASIC stream audit completed; no firmware, receiver, UART, OTG, or RTC modified");
    return 0;
}
