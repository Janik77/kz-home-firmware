#include "protocol.h"

/* Bounded recursive-descent JSON reader. Unknown envelope metadata is validated
 * and skipped, never interpreted as physical intent. Stricter product limit:
 * at most 512 values (including object keys) in one command. */
typedef enum { OBJECT, ARRAY, STRING, BOOL, NUMBER, NIL } kind;
typedef struct { kind type; char *text; int child, next; bool boolean; } token;
typedef struct {
    char *s; size_t n, p; int count;
    token t[512];
} parser;

static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static bool equal(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void copy(char *d, const char *s) { while ((*d++ = *s++)) {} }
static bool digit(char c) { return c >= '0' && c <= '9'; }
static void ws(parser *p)
{ while (p->p < p->n && (p->s[p->p] == ' ' || p->s[p->p] == '\t' ||
          p->s[p->p] == '\r' || p->s[p->p] == '\n')) ++p->p; }

static bool utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], cp; int more;
        if (c < 128) { if (!c) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { more = 1; cp = c & 31; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; cp = c & 15; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; cp = c & 7; }
        else return false;
        int width = more;
        while (more--) {
            if (i == n || (s[i] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (s[i++] & 63);
        }
        if ((width == 2 && cp < 0x800) || (width == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

static int hex4(parser *p)
{
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        if (p->p == p->n) return -1;
        char c = p->s[p->p++];
        int h = digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

static char *string(parser *p)
{
    if (p->p == p->n || p->s[p->p++] != '"') return 0;
    char *start = p->s + p->p, *out = start;
    while (p->p < p->n) {
        unsigned char c = p->s[p->p++];
        if (c == '"') { *out = 0; return start; }
        if (c < 32) return 0;
        if (c == '\\') {
            if (p->p == p->n) return 0;
            c = p->s[p->p++];
            if (c == 'u') {
                int cp = hex4(p);
                if (cp <= 0) return 0; /* NUL is disallowed in this product. */
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (p->p + 2 > p->n || p->s[p->p++] != '\\' || p->s[p->p++] != 'u') return 0;
                    int lo = hex4(p);
                    if (lo < 0xdc00 || lo > 0xdfff) return 0;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return 0;
                if (cp < 0x80) *out++ = (char)cp;
                else if (cp < 0x800) { *out++ = (char)(0xc0 | (cp >> 6)); *out++ = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) {
                    *out++ = (char)(0xe0 | (cp >> 12)); *out++ = (char)(0x80 | ((cp >> 6) & 63)); *out++ = (char)(0x80 | (cp & 63));
                } else {
                    *out++ = (char)(0xf0 | (cp >> 18)); *out++ = (char)(0x80 | ((cp >> 12) & 63));
                    *out++ = (char)(0x80 | ((cp >> 6) & 63)); *out++ = (char)(0x80 | (cp & 63));
                }
                if (out - start > 1024) return 0;
                continue;
            }
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
            default: return 0;
            }
        }
        *out++ = (char)c;
        if (out - start > 1024) return 0;
    }
    return 0;
}

static bool literal(parser *p, const char *s)
{
    while (*s) { if (p->p == p->n || p->s[p->p++] != *s++) return false; }
    return true;
}

static int value(parser *p, int depth)
{
    ws(p);
    if (p->p == p->n || p->count == 512) return -1;
    int index = p->count++;
    token *t = &p->t[index];
    t->child = -1;
    char c = p->s[p->p];
    if (c == '{' || c == '[') {
        if (depth == 8) return -1;
        bool object = c == '{'; char close = object ? '}' : ']';
        t->type = object ? OBJECT : ARRAY;
        ++p->p; ws(p);
        if (p->p < p->n && p->s[p->p] == close) ++p->p;
        else {
            t->child = p->count;
            for (;;) {
                if (object) {
                    int key = value(p, depth + 1);
                    if (key < 0 || p->t[key].type != STRING) return -1;
                    /* Reject duplicate decoded keys, including escaped aliases. */
                    for (int k = t->child; k < key; k = p->t[p->t[k].next].next)
                        if (equal(p->t[k].text, p->t[key].text)) return -1;
                    ws(p);
                    if (p->p == p->n || p->s[p->p++] != ':') return -1;
                }
                if (value(p, depth + 1) < 0) return -1;
                ws(p);
                if (p->p == p->n) return -1;
                char sep = p->s[p->p++];
                if (sep == close) break;
                if (sep != ',') return -1;
                ws(p);
            }
        }
    } else if (c == '"') {
        t->type = STRING; t->text = string(p); if (!t->text) return -1;
    } else if (c == 't' || c == 'f') {
        t->type = BOOL; t->boolean = c == 't';
        if (!literal(p, c == 't' ? "true" : "false")) return -1;
    } else if (c == 'n') {
        t->type = NIL; if (!literal(p, "null")) return -1;
    } else {
        t->type = NUMBER;
        if (c == '-') ++p->p;
        if (p->p == p->n || !digit(p->s[p->p])) return -1;
        if (p->s[p->p++] != '0') while (p->p < p->n && digit(p->s[p->p])) ++p->p;
        if (p->p < p->n && p->s[p->p] == '.') {
            ++p->p; size_t begin = p->p;
            while (p->p < p->n && digit(p->s[p->p])) ++p->p;
            if (p->p == begin) return -1;
        }
        if (p->p < p->n && (p->s[p->p] == 'e' || p->s[p->p] == 'E')) {
            ++p->p;
            if (p->p < p->n && (p->s[p->p] == '+' || p->s[p->p] == '-')) ++p->p;
            size_t begin = p->p;
            while (p->p < p->n && digit(p->s[p->p])) ++p->p;
            if (p->p == begin) return -1;
        }
    }
    t->next = p->count;
    return index;
}

static token *field(parser *p, int object, const char *name)
{
    for (int k = p->t[object].child; k >= 0 && k < p->t[object].next;) {
        int v = p->t[k].next;
        if (equal(p->t[k].text, name)) return &p->t[v];
        k = p->t[v].next;
    }
    return 0;
}

static bool id(token *t)
{ return t && t->type == STRING && length(t->text) > 0 && length(t->text) <= KZ_ID_MAX; }

bool kz_timestamp(const char *s, int64_t *ms)
{
    size_t n = length(s);
    /* UTC RFC3339, optional 1..9 fractional digits, Z or +00:00. */
    if (n < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return false;
    const int positions[] = {0, 5, 8, 11, 14, 17};
    int parts[6] = {0};
    for (int i = 0; i < 6; ++i) for (int j = 0; j < (i == 0 ? 4 : 2); ++j) {
        char c = s[positions[i] + j]; if (!digit(c)) return false;
        parts[i] = parts[i] * 10 + c - '0';
    }
    int y = parts[0], m = parts[1], d = parts[2];
    bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > md[m-1] + (m == 2 && leap) ||
        parts[3] > 23 || parts[4] > 59 || parts[5] > 59) return false;
    size_t pos = 19; int fraction = 0;
    if (s[pos] == '.') {
        ++pos; int digits = 0;
        while (pos < n && digit(s[pos])) {
            if (digits < 3) fraction = fraction * 10 + s[pos] - '0';
            ++pos; if (++digits > 9) return false;
        }
        if (!digits) return false;
        while (digits++ < 3) fraction *= 10;
    }
    if (!equal(s + pos, "Z") && !equal(s + pos, "+00:00")) return false;
    int64_t days = 0;
    for (int year = 1970; year < y; ++year)
        days += 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    for (int month = 1; month < m; ++month) days += md[month-1] + (month == 2 && leap);
    days += d - 1;
    *ms = ((days * 24 + parts[3]) * 3600 + parts[4] * 60 + parts[5]) * 1000 + fraction;
    return true;
}

kz_command kz_parse(char *json, size_t n)
{
    kz_command cmd = {.error = KZ_PROTOCOL};
    if (!n || n > KZ_PAYLOAD_MAX || !utf8((unsigned char *)json, n)) return cmd;
    parser p = {.s = json, .n = n};
    if (value(&p, 0) != 0 || p.t[0].type != OBJECT) return cmd;
    ws(&p); if (p.p != n) return cmd;
    token *cid = field(&p, 0, "command_id"), *corr = field(&p, 0, "correlation_id");
    if (!id(cid) || !id(corr)) return cmd;
    copy(cmd.command_id, cid->text); copy(cmd.correlation_id, corr->text); cmd.identifiable = true;
    token *ts = field(&p, 0, "timestamp"), *state = field(&p, 0, "state");
    token *version = field(&p, 0, "protocol_version");
    if (version && (version->type != STRING || !equal(version->text, "v1"))) return cmd;
    if (!ts || ts->type != STRING || !kz_timestamp(ts->text, &cmd.timestamp_ms) ||
        !state || state->type != OBJECT || state->child < 0) return cmd;
    int fields = 0;
    for (int k = state->child; k < state->next; k = p.t[p.t[k].next].next) {
        if (++fields > 64) return cmd;
        if (!equal(p.t[k].text, "on")) { cmd.error = KZ_UNSUPPORTED; return cmd; }
    }
    token *on = field(&p, (int)(state - p.t), "on");
    if (!on || on->type != BOOL) { cmd.error = KZ_INVALID; return cmd; }
    cmd.on = on->boolean; cmd.error = KZ_OK;
    return cmd;
}

kz_error kz_freshness(int64_t ts, int64_t now)
{ return now - ts > KZ_AGE_MS ? KZ_TIMEOUT : ts - now > KZ_FUTURE_MS ? KZ_PROTOCOL : KZ_OK; }

bool kz_topic_id(const char *s)
{
    size_t n = length(s); if (!n || n > 128) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!(digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
    }
    return true;
}

bool kz_reclaimable(const kz_record *r, int64_t now, int64_t resident)
{
    return r->version && resident >= KZ_DEDUP_MS && now >= r->first_ms &&
           now - r->first_ms >= KZ_DEDUP_MS && now - r->timestamp_ms > KZ_AGE_MS;
}

bool kz_same_command(const kz_record *r, const kz_command *c)
{
    return equal(r->command_id, c->command_id) && equal(r->correlation_id, c->correlation_id) &&
           r->timestamp_ms == c->timestamp_ms && r->on == c->on;
}

const char *kz_error_name(kz_error e)
{
    const char *names[] = {"", "protocol_error", "unsupported_capability", "invalid_value", "timeout", "hardware_failure"};
    return e <= KZ_HARDWARE ? names[e] : "protocol_error";
}
const char *kz_outcome_name(kz_outcome o)
{
    return o == KZ_APPLIED ? "applied" : o == KZ_REJECTED ? "rejected" : o == KZ_FAILED ? "failed" : "accepted";
}

size_t kz_quote(char *out, size_t cap, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '"';
    for (; *s; ++s) {
        unsigned char c = *s;
        size_t need = c < 32 ? 6 : c == '"' || c == '\\' ? 2 : 1;
        if (n + need + 2 > cap) return 0;
        if (c < 32) { out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
        else { if (need == 2) out[n++] = '\\'; out[n++] = (char)c; }
    }
    out[n++] = '"'; out[n] = 0; return n;
}
