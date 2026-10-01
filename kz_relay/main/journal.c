#include "journal.h"
#include "config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

/* Serialized by the device worker. No eviction of live entries or auto-erase. */
static nvs_handle_t handle;
static kz_record records[KZ_JOURNAL_SLOTS];
static int64_t resident_since[KZ_JOURNAL_SLOTS], floor_ms;
static bool healthy;

static bool save(int slot, const kz_record *record)
{
    char key[12]; snprintf(key, sizeof(key), "cmd%03d", slot);
    if (nvs_set_blob(handle, key, record, sizeof(*record)) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        healthy = false; return false;
    }
    records[slot] = *record;
    if (record->first_ms > floor_ms) floor_ms = record->first_ms;
    return true;
}

static esp_err_t bind(const char *key, const char *value)
{
    char stored[129]; size_t size = sizeof(stored);
    esp_err_t err = nvs_get_str(handle, key, stored, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return nvs_set_str(handle, key, value);
    if (err != ESP_OK) return err;
    return strcmp(stored, value) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t journal_init(void)
{
    healthy = false;
    floor_ms = 0;
    memset(records, 0, sizeof(records));
    memset(resident_since, 0, sizeof(resident_since));
    esp_err_t err = nvs_flash_init_partition("commands");
    if (err != ESP_OK) return err;
    err = nvs_open_from_partition("commands", "relay_v1", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if ((err = bind("house", KZ_HOUSE_ID)) != ESP_OK ||
        (err = bind("device", KZ_DEVICE_ID)) != ESP_OK ||
        (err = nvs_commit(handle)) != ESP_OK) return err;
    for (int i = 0; i < KZ_JOURNAL_SLOTS; ++i) {
        char key[12]; snprintf(key, sizeof(key), "cmd%03d", i);
        size_t size = sizeof(kz_record);
        err = nvs_get_blob(handle, key, &records[i], &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) { memset(&records[i], 0, sizeof(kz_record)); continue; }
        kz_record *r = &records[i];
        if (err != ESP_OK || size != sizeof(*r) || r->version != 1 ||
            r->outcome < KZ_PENDING || r->outcome > KZ_FAILED || r->error > KZ_HARDWARE ||
            !memchr(r->command_id, 0, sizeof(r->command_id)) || !r->command_id[0] ||
            !memchr(r->correlation_id, 0, sizeof(r->correlation_id)) || !r->correlation_id[0] ||
            r->first_ms <= 0 || r->timestamp_ms < 0 || r->on > 1) return ESP_ERR_INVALID_STATE;
        if (r->first_ms > floor_ms) floor_ms = r->first_ms;
        resident_since[i] = esp_timer_get_time() / 1000;
        if (r->outcome == KZ_PENDING) {
            /* Power may have failed before OR after GPIO. Never re-execute. */
            kz_record failed = *r; failed.outcome = KZ_FAILED; failed.error = KZ_HARDWARE;
            if (!save(i, &failed)) return ESP_FAIL;
        }
    }
    healthy = true; return ESP_OK;
}

const kz_record *journal_find(const char *id)
{
    for (int i = 0; i < KZ_JOURNAL_SLOTS; ++i)
        if (records[i].version && strcmp(records[i].command_id, id) == 0) return &records[i];
    return NULL;
}

int journal_reserve(const kz_command *c, int64_t now, kz_error error)
{
    if (!healthy || now < floor_ms || journal_find(c->command_id)) return -1;
    int64_t mono = esp_timer_get_time() / 1000;
    for (int i = 0; i < KZ_JOURNAL_SLOTS; ++i) {
        if (records[i].version && !kz_reclaimable(&records[i], now, mono - resident_since[i])) continue;
        kz_record r = {.version = 1, .outcome = error == KZ_OK ? KZ_PENDING : KZ_REJECTED,
            .error = error, .first_ms = now, .timestamp_ms = c->timestamp_ms, .on = c->on};
        memcpy(r.command_id, c->command_id, sizeof(r.command_id));
        memcpy(r.correlation_id, c->correlation_id, sizeof(r.correlation_id));
        if (!save(i, &r)) return -1;
        resident_since[i] = mono; return i;
    }
    return -1;
}

bool journal_finish(int slot, bool applied)
{
    if (!healthy || slot < 0 || slot >= KZ_JOURNAL_SLOTS || records[slot].outcome != KZ_PENDING) return false;
    kz_record r = records[slot];
    r.outcome = applied ? KZ_APPLIED : KZ_FAILED; r.error = applied ? KZ_OK : KZ_HARDWARE;
    return save(slot, &r);
}
const kz_record *journal_get(int slot) { return &records[slot]; }
bool journal_healthy(void) { return healthy; }
int64_t journal_clock_floor(void) { return floor_ms; }
