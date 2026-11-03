#include "fake_platform.h"
#include "protocol.h"
#include "relay.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static kz_record disk[KZ_JOURNAL_SLOTS], pending;
static unsigned disk_size[KZ_JOURNAL_SLOTS];
static int pending_slot = -1;
static bool on;
int fake_apply_count, fake_fail_commit;
int64_t fake_mono_ms;
bool fake_gpio_fails;
int64_t fake_commit_delay_ms;
bool fake_revoke_on_commit, fake_admitted;
void fake_store_blob(int slot, const void *data, unsigned size)
{ assert(size <= sizeof(kz_record)); memset(&disk[slot], 0, sizeof(kz_record)); memcpy(&disk[slot], data, size); disk_size[slot] = size; }
void fake_reset_storage(void)
{
    memset(disk, 0, sizeof(disk)); memset(disk_size, 0, sizeof(disk_size)); fake_reboot();
}
void fake_reboot(void)
{
    pending_slot = -1; fake_fail_commit = 0; fake_apply_count = 0;
    fake_mono_ms = 0; on = false; fake_gpio_fails = false;
    fake_commit_delay_ms = 0; fake_revoke_on_commit = false; fake_admitted = true;
}
int64_t esp_timer_get_time(void) { return fake_mono_ms * 1000; }
esp_err_t relay_init(void) { on = false; return ESP_OK; }
bool relay_apply(bool value)
{
    ++fake_apply_count;
    /* The production engine must have committed its intent before GPIO. */
    bool durable = false;
    for (int i = 0; i < KZ_JOURNAL_SLOTS; ++i) if (disk[i].outcome == KZ_PENDING) durable = true;
    assert(durable);
    if (fake_gpio_fails) return false;
    on = value; return true;
}
bool relay_state(void) { return on; }
bool relay_apply_permitted(bool value, const kz_budget *budget)
{ return kz_budget_valid(budget, esp_timer_get_time()) && relay_apply(value); }
esp_err_t nvs_flash_init_partition(const char *p) { (void)p; return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *p, const char *n, int m, nvs_handle_t *h)
{ (void)p; (void)n; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *n)
{ (void)h; (void)k; (void)v; (void)n; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *n)
{
    (void)h; int slot = atoi(k + 3); assert(slot >= 0 && slot < KZ_JOURNAL_SLOTS);
    if (!disk[slot].version) return ESP_ERR_NVS_NOT_FOUND;
    assert(*n >= disk_size[slot]); *n = disk_size[slot]; memcpy(v, &disk[slot], *n); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n)
{
    (void)h; assert(n == sizeof(pending)); pending_slot = atoi(k + 3); memcpy(&pending, v, n); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    fake_mono_ms += fake_commit_delay_ms;
    if (fake_revoke_on_commit) fake_admitted = false;
    if (fake_fail_commit > 0 && --fake_fail_commit == 0) return ESP_FAIL;
    if (pending_slot >= 0) { disk[pending_slot] = pending; disk_size[pending_slot] = sizeof(pending); pending_slot = -1; }
    return ESP_OK;
}
