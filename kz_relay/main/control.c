#include "control.h"
#include "journal.h"
#include "esp_random.h"
#include <psa/crypto.h>
#include <stdio.h>
#include <string.h>

static void random_id(char out[33])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[16]; esp_fill_random(bytes, sizeof(bytes));
    for (int i = 0; i < 16; ++i) { out[2*i] = hex[bytes[i] >> 4]; out[2*i+1] = hex[bytes[i] & 15]; }
    out[32] = 0;
}
bool control_hash(const char *raw, size_t size, char out[65])
{
    unsigned char hash[32]; size_t written;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_compute(PSA_ALG_SHA_256,
        (const unsigned char *)raw, size, hash, sizeof(hash), &written) != PSA_SUCCESS || written != 32) return false;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) { out[2*i] = hex[hash[i] >> 4]; out[2*i+1] = hex[hash[i] & 15]; }
    out[64] = 0; return true;
}
void control_init(kz_control *s)
{ memset(s, 0, sizeof(*s)); random_id(s->boot); }
void control_cancel(kz_control *s, int64_t wall)
{
    if (s->waiting && !journal_find(s->command.command_id)) {
        if (wall <= 0) wall = s->received_wall_ms;
        if (wall > 0) journal_reserve(&s->command, wall, KZ_TIMEOUT);
    }
    s->waiting = false;
}
void control_session(kz_control *s, unsigned generation, int64_t wall)
{
    control_cancel(s, wall); random_id(s->session); s->generation = generation;
}
bool control_candidate(kz_control *s, char *raw, size_t n, unsigned generation, int64_t wall, int64_t mono)
{
    char hash[65]; kz_command command;
    s->duplicate = NULL;
    if (s->waiting || generation != s->generation || wall <= 0 || mono < 0 || !journal_healthy() ||
        !control_hash(raw, n, hash) || !kz_parse_control(raw, n, hash, &command)) return false;
    const kz_record *previous = journal_find(command.command_id);
    if (previous) {
        if (kz_same_command(previous, &command)) s->duplicate = previous;
        return false;
    }
    s->command = command; random_id(s->nonce);
    s->received_wall_ms = wall; s->start_us = mono; s->waiting = true;
    return true;
}
bool control_request(const kz_control *s, char *out, size_t size)
{
    if (!s->waiting) return false;
    int n = snprintf(out, size, "{\"control_version\":1,\"type\":\"permit_request\","
        "\"execution_profile\":\"deadline-permit-v1\",\"core_epoch\":\"%s\",\"command_id\":\"%s\","
        "\"boot_id\":\"%s\",\"session_id\":\"%s\",\"nonce\":\"%s\",\"intent_sha256\":\"%s\"}",
        s->command.epoch, s->command.command_id, s->boot, s->session, s->nonce, s->command.digest);
    return n > 0 && (size_t)n < size;
}
bool control_reply(kz_control *s, char *raw, size_t n, unsigned generation, int64_t mono, kz_budget *budget)
{
    kz_permit p;
    if (!s->waiting || generation != s->generation || mono < s->start_us ||
        mono - s->start_us >= 2000000 || !kz_parse_permit(raw, n, &p) ||
        strcmp(p.epoch, s->command.epoch) || strcmp(p.command_id, s->command.command_id) ||
        strcmp(p.boot, s->boot) || strcmp(p.session, s->session) || strcmp(p.nonce, s->nonce) ||
        strcmp(p.digest, s->command.digest)) return false;
    if (p.denied) { control_cancel(s, s->received_wall_ms); return false; }
    if (p.expires_us != s->command.expires_us || p.core_us < s->command.created_us) return false;
    /* Consume before journal/GPIO; duplicate replies cannot execute again. */
    s->waiting = false;
    budget->start_us = s->start_us; budget->remaining_us = p.remaining_us;
    return true;
}
