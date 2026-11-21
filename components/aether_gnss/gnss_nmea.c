#include "gnss_nmea.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LINE_CAP 192
#define FIELD_CAP 32
#define GSV_STREAMS 16
#define GSV_MAX_PAGES 32
#define SYSTEM_COUNT ((unsigned)GNSS_SYSTEM_SBAS + 1U)

typedef struct {
    bool seen, valid;
    uint64_t received_ms;
    double latitude, longitude;
} position_sample_t;

typedef struct {
    bool allocated, complete, cycle_active;
    gnss_system_t system;
    char talker[2];
    int signal_id;
    unsigned pages, total;
    uint32_t page_mask;
    uint64_t cycle_ms, complete_ms;
    gnss_satellite_t pending[GNSS_MAX_SATELLITES];
    gnss_satellite_t published[GNSS_MAX_SATELLITES];
    unsigned published_count;
} gsv_stream_t;

typedef struct {
    bool seen;
    uint64_t received_ms;
    unsigned dimension, count;
    uint16_t prns[12];
} used_system_t;

struct gnss_nmea_parser {
    gnss_nmea_snapshot_t value;
    char line[LINE_CAP];
    size_t line_len;
    bool collecting, carriage_return;
    position_sample_t rmc, gga;
    bool time_seen, altitude_seen, speed_seen, course_seen, hdop_seen;
    uint64_t altitude_ms, speed_ms, course_ms, hdop_ms;
    gsv_stream_t streams[GSV_STREAMS];
    used_system_t used[SYSTEM_COUNT];
};

static bool fresh(uint64_t now, uint64_t then, uint64_t limit)
{
    return now >= then && now - then <= limit;
}

static bool uint_field(const char *s, unsigned max, unsigned *out)
{
    if (!s || !*s) return false;
    unsigned n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        unsigned digit = (unsigned)(*s - '0');
        if (n > max / 10U || (n == max / 10U && digit > max % 10U)) return false;
        n = n * 10U + digit;
    }
    *out = n;
    return true;
}

/* NMEA decimals are not C floating point literals: reject NaN, Inf, hex,
 * exponents, whitespace, and a sign without digits before calling strtod. */
static bool decimal_field(const char *s, double min, double max, double *out)
{
    if (!s || !*s) return false;
    const char *p = s;
    if (*p == '-' || *p == '+') ++p;
    unsigned digits = 0, dots = 0;
    for (; *p; ++p) {
        if (*p >= '0' && *p <= '9') ++digits;
        else if (*p == '.' && ++dots == 1) { }
        else return false;
    }
    if (!digits || p[-1] == '.') return false;
    char *end = NULL;
    double n = strtod(s, &end);
    if (!end || *end || !isfinite(n) || n < min || n > max) return false;
    *out = n;
    return true;
}

static bool optional_decimal(const char *s, double min, double max, double *out, bool *present)
{
    *present = s && *s;
    return !*present || decimal_field(s, min, max, out);
}

static bool clock_field(const char *s, uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (!s || strlen(s) < 6) return false;
    for (unsigned i = 0; i < 6; ++i) if (s[i] < '0' || s[i] > '9') return false;
    unsigned h = (unsigned)(s[0] - '0') * 10U + (unsigned)(s[1] - '0');
    unsigned m = (unsigned)(s[2] - '0') * 10U + (unsigned)(s[3] - '0');
    unsigned sec = (unsigned)(s[4] - '0') * 10U + (unsigned)(s[5] - '0');
    if (h > 23 || m > 59 || sec > 59) return false;
    if (s[6]) {
        if (s[6] != '.' || !s[7]) return false;
        for (const char *p = s + 7; *p; ++p) if (*p < '0' || *p > '9') return false;
    }
    *hour = (uint8_t)h; *minute = (uint8_t)m; *second = (uint8_t)sec;
    return true;
}

static bool date_field(const char *s, uint16_t *year, uint8_t *month, uint8_t *day)
{
    if (!s || strlen(s) != 6) return false;
    unsigned n;
    if (!uint_field(s, 999999, &n)) return false;
    unsigned d = n / 10000U, m = (n / 100U) % 100U, y = n % 100U;
    y += y >= 80U ? 1900U : 2000U;
    static const unsigned days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12 || d < 1) return false;
    unsigned limit = days[m];
    if (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ++limit;
    if (d > limit) return false;
    *year = (uint16_t)y; *month = (uint8_t)m; *day = (uint8_t)d;
    return true;
}

static bool coordinate(const char *s, const char *hemisphere, bool longitude, double *out)
{
    if (!s || !hemisphere || hemisphere[0] == '\0' || hemisphere[1] != '\0') return false;
    size_t integer_digits = strcspn(s, ".");
    if (integer_digits != (longitude ? 5U : 4U)) return false;
    for (size_t i = 0; i < integer_digits; ++i) if (s[i] < '0' || s[i] > '9') return false;
    double raw;
    if (!decimal_field(s, 0, longitude ? 18000 : 9000, &raw)) return false;
    unsigned deg = (unsigned)(raw / 100);
    double minutes = raw - (double)deg * 100;
    unsigned max = longitude ? 180U : 90U;
    if (minutes >= 60 || deg > max || (deg == max && minutes != 0)) return false;
    char h = hemisphere[0];
    if (longitude ? (h != 'E' && h != 'W') : (h != 'N' && h != 'S')) return false;
    *out = (double)deg + minutes / 60;
    if (h == 'W' || h == 'S') *out = -*out;
    return true;
}

static bool coordinates(char **f, unsigned first, bool required, double *lat, double *lon)
{
    bool any = *f[first] || *f[first + 1] || *f[first + 2] || *f[first + 3];
    if (!any) return !required;
    return coordinate(f[first], f[first + 1], false, lat) &&
           coordinate(f[first + 2], f[first + 3], true, lon);
}

static gnss_system_t talker_system(const char *s)
{
    if (!strncmp(s, "GP", 2)) return GNSS_SYSTEM_GPS;
    if (!strncmp(s, "GL", 2)) return GNSS_SYSTEM_GLONASS;
    if (!strncmp(s, "BD", 2) || !strncmp(s, "GB", 2)) return GNSS_SYSTEM_BEIDOU;
    if (!strncmp(s, "GA", 2)) return GNSS_SYSTEM_GALILEO;
    if (!strncmp(s, "GQ", 2) || !strncmp(s, "QZ", 2)) return GNSS_SYSTEM_QZSS;
    return GNSS_SYSTEM_UNKNOWN;
}

static gnss_system_t system_id(unsigned id)
{
    switch (id) {
    case 1: return GNSS_SYSTEM_GPS;
    case 2: return GNSS_SYSTEM_GLONASS;
    case 3: return GNSS_SYSTEM_GALILEO;
    case 4: return GNSS_SYSTEM_BEIDOU;
    case 5: return GNSS_SYSTEM_QZSS;
    default: return GNSS_SYSTEM_UNKNOWN;
    }
}

static void update_position(gnss_nmea_parser_t *p, uint64_t now)
{
    bool r = p->rmc.seen && fresh(now, p->rmc.received_ms, GNSS_NMEA_FIX_TIMEOUT_MS);
    bool g = p->gga.seen && fresh(now, p->gga.received_ms, GNSS_NMEA_FIX_TIMEOUT_MS);
    /* Fresh explicit no-fix from either source vetoes an old valid position.
     * It is safer to wait for a coherent next epoch than flash a false fix. */
    bool valid = (r || g) && (!r || p->rmc.valid) && (!g || p->gga.valid);
    p->value.position_valid = valid;
    if (valid) {
        const position_sample_t *s = r ? &p->rmc : &p->gga;
        p->value.latitude_deg = s->latitude;
        p->value.longitude_deg = s->longitude;
        p->value.last_fix_ms = s->received_ms;
    }
}

static bool parse_rmc(gnss_nmea_parser_t *p, char **f, unsigned count, uint64_t now)
{
    if (count < 10 || (strcmp(f[2], "A") && strcmp(f[2], "V"))) return false;
    bool valid = !strcmp(f[2], "A");
    if (count > 12 && *f[12]) {
        if (strlen(f[12]) != 1 || !strchr("ADEFMNPRS", *f[12])) return false;
        if (*f[12] == 'N' || *f[12] == 'M' || *f[12] == 'E') valid = false;
    }
    uint8_t h = 0, m = 0, s = 0, month = 0, day = 0;
    uint16_t year = 0;
    bool clock_ok = *f[1] && clock_field(f[1], &h, &m, &s);
    bool date_ok = *f[9] && date_field(f[9], &year, &month, &day);
    if ((*f[1] && !clock_ok) || (*f[9] && !date_ok) || (valid && (!clock_ok || !date_ok))) return false;
    double lat = 0, lon = 0, speed = 0, course = 0;
    bool speed_present, course_present;
    if (!coordinates(f, 3, valid, &lat, &lon) ||
        !optional_decimal(f[7], 0, 2000, &speed, &speed_present) ||
        !optional_decimal(f[8], 0, 360, &course, &course_present)) return false;
    p->rmc = (position_sample_t){true, valid, now, lat, lon};
    p->time_seen = valid && clock_ok && date_ok;
    p->value.time_valid = p->time_seen;
    p->value.date_valid = p->time_seen;
    if (p->time_seen) {
        p->value.year = year; p->value.month = month; p->value.day = day;
        p->value.hour = h; p->value.minute = m; p->value.second = s;
        p->value.last_time_ms = now;
    }
    p->speed_seen = valid && speed_present;
    p->course_seen = valid && course_present;
    p->speed_ms = now; p->course_ms = now;
    if (speed_present) p->value.speed_kmh = (float)(speed * 1.852);
    if (course_present) p->value.course_deg = course == 360 ? 0 : (float)course;
    update_position(p, now);
    return true;
}

static bool parse_gga(gnss_nmea_parser_t *p, char **f, unsigned count, uint64_t now)
{
    if (count < 10) return false;
    unsigned quality, satellites;
    uint8_t h, m, s;
    if (!uint_field(f[6], 8, &quality) || !uint_field(f[7], 99, &satellites) ||
        (*f[1] && !clock_field(f[1], &h, &m, &s))) return false;
    bool valid = quality > 0 && quality < 6;
    if (valid && !*f[1]) return false;
    double lat = 0, lon = 0, hdop = 0, altitude = 0;
    bool hdop_present, altitude_present;
    if (!coordinates(f, 2, valid, &lat, &lon) ||
        !optional_decimal(f[8], 0, 999, &hdop, &hdop_present) ||
        !optional_decimal(f[9], -20000, 100000, &altitude, &altitude_present)) return false;
    if (altitude_present && (count < 11 || strcmp(f[10], "M"))) return false;
    p->gga = (position_sample_t){true, valid, now, lat, lon};
    p->value.fix_quality = (uint8_t)quality;
    p->value.satellites_used = valid ? (uint8_t)satellites : 0;
    p->hdop_seen = valid && hdop_present; p->hdop_ms = now;
    p->altitude_seen = valid && altitude_present; p->altitude_ms = now;
    if (hdop_present) p->value.hdop = (float)hdop;
    if (altitude_present) p->value.altitude_m = (float)altitude;
    update_position(p, now);
    return true;
}

static bool parse_gsa(gnss_nmea_parser_t *p, char **f, unsigned count, uint64_t now)
{
    if (count < 18 || count > 19 || (strcmp(f[1], "A") && strcmp(f[1], "M"))) return false;
    unsigned dimension;
    if (!uint_field(f[2], 3, &dimension) || dimension < 1) return false;
    gnss_system_t system = talker_system(f[0]);
    /* Legacy GNGSA without a systemId is genuinely ambiguous: GPS and BeiDou
     * PRNs overlap on CASIC receivers. Keep its set UNKNOWN rather than mark
     * the wrong constellation as used. GP/BD/GL GSA and 4.1 systemId are exact. */
    if (count == 19 && *f[18]) {
        unsigned id;
        if (!uint_field(f[18], 99, &id)) return false;
        system = system_id(id);
    }
    used_system_t next = {.seen = true, .received_ms = now, .dimension = dimension};
    for (unsigned i = 3; i < 15; ++i) {
        if (!*f[i]) continue;
        unsigned prn;
        if (!uint_field(f[i], 999, &prn) || !prn) return false;
        bool duplicate = false;
        for (unsigned j = 0; j < next.count; ++j) if (next.prns[j] == prn) duplicate = true;
        if (!duplicate && dimension > 1) next.prns[next.count++] = (uint16_t)prn;
    }
    double dop[3] = {0}; bool present[3];
    for (unsigned i = 0; i < 3; ++i)
        if (!optional_decimal(f[15 + i], 0, 999, &dop[i], &present[i])) return false;
    p->used[(unsigned)system] = next;
    if (present[1] && dimension > 1) {
        p->value.hdop = (float)dop[1]; p->hdop_seen = true; p->hdop_ms = now;
    }
    return true;
}

static gsv_stream_t *get_stream(gnss_nmea_parser_t *p, gnss_system_t system,
                                 const char *talker, int signal, uint64_t now)
{
    gsv_stream_t *candidate = NULL;
    for (unsigned i = 0; i < GSV_STREAMS; ++i) {
        gsv_stream_t *s = &p->streams[i];
        if (s->allocated && s->system == system && !memcmp(s->talker, talker, 2) &&
            s->signal_id == signal) return s;
        if (!candidate || !s->allocated ||
            (!fresh(now, s->complete_ms, GNSS_NMEA_SATELLITE_TIMEOUT_MS) &&
             s->complete_ms < candidate->complete_ms)) candidate = s;
    }
    memset(candidate, 0, sizeof(*candidate));
    candidate->allocated = true; candidate->system = system;
    memcpy(candidate->talker, talker, 2); candidate->signal_id = signal;
    return candidate;
}

static bool parse_gsv(gnss_nmea_parser_t *p, char **f, unsigned count, uint64_t now)
{
    if (count < 4) return false;
    unsigned pages, page, total;
    if (!uint_field(f[1], GSV_MAX_PAGES, &pages) || !pages ||
        !uint_field(f[2], pages, &page) || !page ||
        !uint_field(f[3], 128, &total) || pages != (total ? (total + 3U) / 4U : 1U)) return false;
    unsigned remaining = count - 4U, groups = remaining / 4U;
    int signal = -1;
    if (remaining % 4U == 1U && *f[count - 1]) {
        unsigned id;
        if (!uint_field(f[count - 1], 255, &id)) return false;
        signal = (int)id;
    } else if (remaining % 4U != 0 && remaining % 4U != 1) return false;
    unsigned expected = total > (page - 1U) * 4U ? total - (page - 1U) * 4U : 0;
    if (expected > 4) expected = 4;
    if (groups < expected || groups > 4) return false;
    /* Some receivers retain all four slots on the last page, including a
     * zero-satellite page. Accept only entirely empty trailing slot padding;
     * an optional final signalId has already been separated by field count. */
    for (unsigned i = expected; i < groups; ++i) {
        char **padding = &f[4 + i * 4U];
        if (*padding[0] || *padding[1] || *padding[2] || *padding[3]) return false;
    }
    gnss_system_t system = talker_system(f[0]);
    gnss_satellite_t local[4] = {{0}};
    for (unsigned i = 0; i < expected; ++i) {
        unsigned prn, elevation = 0, azimuth = 0, snr = 0;
        char **g = &f[4 + i * 4U];
        if (!uint_field(g[0], 999, &prn) || !prn ||
            (*g[1] && !uint_field(g[1], 90, &elevation)) ||
            (*g[2] && !uint_field(g[2], 359, &azimuth)) ||
            (*g[3] && !uint_field(g[3], 99, &snr))) return false;
        local[i] = (gnss_satellite_t){.system = system, .prn = (uint16_t)prn,
            .azimuth_deg = (uint16_t)azimuth, .elevation_deg = (uint8_t)elevation,
            .snr_dbhz = (uint8_t)snr, .signal_valid = *g[3] != '\0',
            .geometry_valid = *g[1] != '\0' && *g[2] != '\0'};
    }
    gsv_stream_t *stream = get_stream(p, system, f[0], signal, now);
    if (page == 1) {
        stream->page_mask = 0; stream->pages = pages; stream->total = total;
        stream->cycle_ms = now; stream->cycle_active = true;
        memset(stream->pending, 0, sizeof(stream->pending));
    }
    if (!stream->cycle_active || !fresh(now, stream->cycle_ms, GNSS_NMEA_SATELLITE_TIMEOUT_MS) ||
        stream->pages != pages || stream->total != total || !stream->pages) return false;
    unsigned base = (page - 1U) * 4U;
    for (unsigned i = 0; i < expected && base + i < GNSS_MAX_SATELLITES; ++i)
        stream->pending[base + i] = local[i];
    stream->page_mask |= UINT32_C(1) << (page - 1U);
    uint32_t all = pages == 32 ? UINT32_MAX : (UINT32_C(1) << pages) - 1U;
    if (stream->page_mask == all) {
        stream->published_count = total > GNSS_MAX_SATELLITES ? GNSS_MAX_SATELLITES : total;
        memcpy(stream->published, stream->pending, sizeof(stream->published));
        stream->complete_ms = now; stream->complete = true;
        stream->cycle_active = false;
        stream->page_mask = 0;
        p->value.last_satellite_ms = now;
    }
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void parse_line(gnss_nmea_parser_t *p, uint64_t now)
{
    p->line[p->line_len] = '\0';
    char *star = strchr(p->line, '*');
    if (!star || strlen(star) != 3 || hex_digit(star[1]) < 0 || hex_digit(star[2]) < 0) {
        ++p->value.malformed_sentences; return;
    }
    unsigned checksum = 0;
    for (const char *s = p->line; s != star; ++s) checksum ^= (unsigned char)*s;
    if (checksum != (unsigned)(hex_digit(star[1]) * 16 + hex_digit(star[2]))) {
        ++p->value.bad_checksums; return;
    }
    *star = '\0';
    char *fields[FIELD_CAP]; unsigned count = 1;
    fields[0] = p->line;
    for (char *s = p->line; *s; ++s) {
        if (*s == ',') {
            if (count == FIELD_CAP) { ++p->value.malformed_sentences; return; }
            *s = '\0'; fields[count++] = s + 1;
        }
    }
    if (strlen(fields[0]) != 5) { ++p->value.malformed_sentences; return; }
    for (unsigned i = 0; i < 5; ++i)
        if (fields[0][i] < 'A' || fields[0][i] > 'Z') { ++p->value.malformed_sentences; return; }
    const char *kind = fields[0] + 2;
    bool valid = true;
    if (!strcmp(kind, "RMC")) valid = parse_rmc(p, fields, count, now);
    else if (!strcmp(kind, "GGA")) valid = parse_gga(p, fields, count, now);
    else if (!strcmp(kind, "GSA")) valid = parse_gsa(p, fields, count, now);
    else if (!strcmp(kind, "GSV")) valid = parse_gsv(p, fields, count, now);
    if (!valid) { ++p->value.malformed_sentences; return; }
    ++p->value.accepted_sentences; p->value.last_sentence_ms = now;
}

gnss_nmea_parser_t *gnss_nmea_new(void)
{
    return calloc(1, sizeof(gnss_nmea_parser_t));
}

void gnss_nmea_free(gnss_nmea_parser_t *p) { free(p); }

void gnss_nmea_reset(gnss_nmea_parser_t *p)
{
    if (p) memset(p, 0, sizeof(*p));
}

void gnss_nmea_feed(gnss_nmea_parser_t *p, const char *data, size_t len, uint64_t now)
{
    if (!p || (!data && len)) return;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c == '$') {
            if (p->collecting && p->line_len) ++p->value.malformed_sentences;
            p->collecting = true; p->carriage_return = false; p->line_len = 0;
        } else if (!p->collecting) {
            continue;
        } else if (c == '\n') {
            parse_line(p, now); p->collecting = false; p->line_len = 0;
        } else if (c == '\r' && !p->carriage_return) {
            p->carriage_return = true;
        } else if (p->carriage_return || c < 32 || c > 126 || p->line_len >= LINE_CAP - 1U) {
            ++p->value.malformed_sentences; p->collecting = false; p->line_len = 0;
        } else {
            p->line[p->line_len++] = (char)c;
        }
    }
}

void gnss_nmea_get_snapshot(gnss_nmea_parser_t *p, uint64_t now, gnss_nmea_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!p) return;
    update_position(p, now);
    *out = p->value;
    out->time_valid = p->time_seen && fresh(now, out->last_time_ms, GNSS_NMEA_FIX_TIMEOUT_MS);
    out->date_valid = out->time_valid;
    out->altitude_valid = out->position_valid && p->altitude_seen &&
        fresh(now, p->altitude_ms, GNSS_NMEA_FIX_TIMEOUT_MS);
    out->speed_valid = out->position_valid && p->speed_seen && fresh(now, p->speed_ms, GNSS_NMEA_FIX_TIMEOUT_MS);
    out->course_valid = out->position_valid && p->course_seen && fresh(now, p->course_ms, GNSS_NMEA_FIX_TIMEOUT_MS);
    out->hdop_valid = out->position_valid && p->hdop_seen && fresh(now, p->hdop_ms, GNSS_NMEA_FIX_TIMEOUT_MS);
    out->fix_dimension = 0; out->satellite_count = 0;
    unsigned used_count = 0;
    for (unsigned i = 0; i < SYSTEM_COUNT; ++i) {
        const used_system_t *u = &p->used[i];
        if (!u->seen || !fresh(now, u->received_ms, GNSS_NMEA_SATELLITE_TIMEOUT_MS)) continue;
        if (u->dimension > out->fix_dimension) out->fix_dimension = (uint8_t)u->dimension;
        used_count += u->count;
    }
    if (!p->gga.seen || !fresh(now, p->gga.received_ms, GNSS_NMEA_FIX_TIMEOUT_MS))
        out->satellites_used = out->position_valid ? (uint8_t)(used_count > 99 ? 99 : used_count) : 0;
    if (!out->position_valid) out->satellites_used = 0;
    if (!p->gga.seen || !fresh(now, p->gga.received_ms, GNSS_NMEA_FIX_TIMEOUT_MS)) out->fix_quality = 0;
    for (unsigned i = 0; i < GSV_STREAMS; ++i) {
        const gsv_stream_t *s = &p->streams[i];
        if (!s->complete || !fresh(now, s->complete_ms, GNSS_NMEA_SATELLITE_TIMEOUT_MS)) continue;
        for (unsigned j = 0; j < s->published_count; ++j) {
            const gnss_satellite_t *sat = &s->published[j];
            if (!sat->prn) continue;
            unsigned slot = 0;
            while (slot < out->satellite_count &&
                (out->satellites[slot].system != sat->system || out->satellites[slot].prn != sat->prn)) ++slot;
            if (slot == out->satellite_count) {
                if (slot == GNSS_MAX_SATELLITES) continue;
                out->satellites[slot] = *sat; ++out->satellite_count;
            } else {
                gnss_satellite_t *dest = &out->satellites[slot];
                if (sat->geometry_valid) {
                    dest->azimuth_deg = sat->azimuth_deg; dest->elevation_deg = sat->elevation_deg;
                    dest->geometry_valid = true;
                }
                if (sat->signal_valid && (!dest->signal_valid || sat->snr_dbhz > dest->snr_dbhz)) {
                    dest->snr_dbhz = sat->snr_dbhz; dest->signal_valid = true;
                }
            }
        }
    }
    for (unsigned i = 0; i < out->satellite_count; ++i) {
        gnss_satellite_t *sat = &out->satellites[i];
        const used_system_t *u = &p->used[(unsigned)sat->system];
        sat->used = false;
        if (!u->seen || !fresh(now, u->received_ms, GNSS_NMEA_SATELLITE_TIMEOUT_MS)) continue;
        for (unsigned j = 0; j < u->count; ++j) if (sat->prn == u->prns[j]) sat->used = true;
    }
}
