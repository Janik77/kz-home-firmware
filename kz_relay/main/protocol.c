#include "protocol.h"

/* Bounded recursive-descent JSON reader. Unknown envelope metadata is validated
 * and skipped, never interpreted as physical intent. Stricter product limit:
 * at most 512 values (including object keys) in one command. */
typedef enum { OBJECT, ARRAY, STRING, BOOL, NUMBER, NIL } kind;
typedef struct { kind type; char *text; int child, next; bool boolean; size_t size; } token;
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
        t->text = p->s + p->p;
        size_t begin_number = p->p;
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
        t->size = p->p - begin_number;
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
    return r->version && r->outcome != KZ_PENDING && resident >= KZ_DEDUP_MS && now >= r->first_ms &&
           now - r->first_ms >= KZ_DEDUP_MS &&
           (r->version == 2 || now - r->timestamp_ms > KZ_AGE_MS);
}

bool kz_same_command(const kz_record *r, const kz_command *c)
{
    return c->control && r->expires_us > 0 && equal(r->command_id, c->command_id) &&
           equal(r->correlation_id, c->correlation_id) && r->on == c->on &&
           r->created_us == c->created_us && r->expires_us == c->expires_us &&
           equal(r->epoch, c->epoch) && equal(r->digest, c->digest);
}

static bool text_is(token *t, const char *s)
{ return t && t->type == STRING && equal(t->text, s); }
static bool hex_text(const char *s, size_t n)
{
    if (length(s) != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (!digit(s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) return false;
    return true;
}
static bool uuid_text(token *t)
{
    if (!t || t->type != STRING || length(t->text) != 36) return false;
    for (int i = 0; i < 36; ++i) {
        char c = t->text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!digit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}
static bool exact(parser *p, const char *const *names, size_t count)
{
    size_t found = 0;
    if (p->t[0].type != OBJECT) return false;
    for (int k = p->t[0].child; k >= 0 && k < p->t[0].next; k = p->t[p->t[k].next].next) {
        bool known = false;
        for (size_t j = 0; j < count; ++j) if (equal(p->t[k].text, names[j])) known = true;
        if (!known) return false;
        ++found;
    }
    return found == count;
}
static bool uint_value(token *t, int64_t max, int64_t *out)
{
    if (!t || t->type != NUMBER || !t->size) return false;
    int64_t n = 0;
    for (size_t i = 0; i < t->size; ++i) {
        if (!digit(t->text[i]) || n > (max - (t->text[i] - '0')) / 10) return false;
        n = n * 10 + t->text[i] - '0';
        if (n > max) return false;
    }
    *out = n; return true;
}
static bool time_us(token *t, int64_t *out)
{
    int64_t ms;
    if (!t || t->type != STRING || !kz_timestamp(t->text, &ms)) return false;
    unsigned fraction = 0, digits = 0;
    if (t->text[19] == '.') {
        for (int i = 20; digit(t->text[i]); ++i) {
            if (++digits > 6) return false;
            fraction = fraction * 10 + (unsigned)(t->text[i] - '0');
        }
    }
    while (digits++ < 6) fraction *= 10;
    *out = ms * 1000 + fraction % 1000; return true;
}
static bool document(parser *p, char *json, size_t n)
{
    if (!n || n > 4096 || !utf8((unsigned char *)json, n)) return false;
    p->s = json; p->n = n; p->p = 0; p->count = 0;
    if (value(p, 0) != 0) return false;
    ws(p); return p->p == n;
}
bool kz_parse_control(char *json, size_t n, const char digest[65], kz_command *out)
{
    *out = (kz_command){0}; parser p;
    const char *const names[] = {"protocol_version", "execution_profile", "core_epoch", "command_id",
        "correlation_id", "timestamp", "expires_at", "state"};
    if (!document(&p, json, n) || !exact(&p, names, 8) || !hex_text(digest, 64)) return false;
    token *epoch = field(&p, 0, "core_epoch"), *cid = field(&p, 0, "command_id"),
        *corr = field(&p, 0, "correlation_id"), *state = field(&p, 0, "state");
    if (!text_is(field(&p, 0, "protocol_version"), "v1") ||
        !text_is(field(&p, 0, "execution_profile"), "deadline-permit-v1") ||
        !uuid_text(epoch) || !uuid_text(cid) || !uuid_text(corr) ||
        !time_us(field(&p, 0, "timestamp"), &out->created_us) ||
        !time_us(field(&p, 0, "expires_at"), &out->expires_us) ||
        out->expires_us <= out->created_us || out->expires_us - out->created_us > 30000000 ||
        !state || state->type != OBJECT || state->child < 0) return false;
    int key = state->child, val = p.t[key].next;
    if (!equal(p.t[key].text, "on") || p.t[val].type != BOOL || p.t[val].next != state->next) return false;
    copy(out->command_id, cid->text); copy(out->correlation_id, corr->text);
    copy(out->epoch, epoch->text); copy(out->digest, digest);
    out->timestamp_ms = out->created_us / 1000; out->on = p.t[val].boolean;
    out->control = out->identifiable = true; out->error = KZ_OK; return true;
}
bool kz_parse_permit(char *json, size_t n, kz_permit *out)
{
    *out = (kz_permit){0}; parser p;
    const char *const granted[] = {"control_version", "type", "execution_profile", "core_epoch",
        "command_id", "boot_id", "session_id", "nonce", "intent_sha256", "core_time", "expires_at", "remaining_us"};
    const char *const denied[] = {"control_version", "type", "execution_profile", "core_epoch",
        "command_id", "boot_id", "session_id", "nonce", "intent_sha256", "reason"};
    if (!document(&p, json, n)) return false;
    out->denied = text_is(field(&p, 0, "type"), "permit_denied");
    if (!(out->denied ? exact(&p, denied, 10) : exact(&p, granted, 12))) return false;
    int64_t version;
    token *epoch = field(&p, 0, "core_epoch"), *cid = field(&p, 0, "command_id");
    if (!uint_value(field(&p, 0, "control_version"), 1, &version) || version != 1 ||
        !text_is(field(&p, 0, "execution_profile"), "deadline-permit-v1") ||
        !uuid_text(epoch) || !uuid_text(cid)) return false;
    const char *names[] = {"boot_id", "session_id", "nonce", "intent_sha256"};
    char *dest[] = {out->boot, out->session, out->nonce, out->digest};
    for (int i = 0; i < 4; ++i) {
        token *t = field(&p, 0, names[i]);
        if (!t || t->type != STRING || !hex_text(t->text, i == 3 ? 64 : 32)) return false;
        copy(dest[i], t->text);
    }
    copy(out->epoch, epoch->text); copy(out->command_id, cid->text);
    if (out->denied) return text_is(field(&p, 0, "reason"), "unavailable") ||
        text_is(field(&p, 0, "reason"), "expired") || text_is(field(&p, 0, "reason"), "invalid");
    return text_is(field(&p, 0, "type"), "permit_grant") &&
        time_us(field(&p, 0, "core_time"), &out->core_us) &&
        time_us(field(&p, 0, "expires_at"), &out->expires_us) &&
        uint_value(field(&p, 0, "remaining_us"), 2000000, &out->remaining_us) &&
        out->remaining_us > 0 && out->core_us < out->expires_us &&
        out->remaining_us <= out->expires_us - out->core_us;
}
bool kz_budget_valid(const kz_budget *b, int64_t now)
{
    if (b->admitted && !b->admitted(b->admission_context)) return false;
    if (!b->qualified || b->start_us < 0 || now < b->start_us || b->remaining_us <= 0 ||
        b->remaining_us > 2000000 || b->core_ppm >= 1000000 || b->device_ppm >= 1000000) return false;
    int64_t elapsed = now - b->start_us;
    if (elapsed > 2000000) return false;
    int64_t divisor = 1000000 - b->device_ppm;
    int64_t upper = (elapsed * (1000000 + b->core_ppm) + divisor - 1) / divisor;
    int64_t gpio = ((int64_t)b->gpio_us * (1000000 + b->core_ppm) + 999999) / 1000000;
    return upper + b->epsilon_us + gpio < b->remaining_us;
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
