#include "command_engine.h"
#include "journal.h"
#include "relay.h"
#include "esp_timer.h"

kz_result kz_process(const kz_command *c, int64_t now, bool retained)
{
    /* Legacy physical set is never an execution path in v0.12. */
    (void)c; (void)now; (void)retained;
    return (kz_result){0};
}

kz_result kz_process_permit(const kz_command *c, int64_t now, const kz_budget *budget)
{
    kz_result result = {0};
    if (!c->control || !c->identifiable || now <= 0 || !journal_healthy()) return result;
    const kz_record *previous = journal_find(c->command_id);
    if (previous) {
        /* Invalid state parsing can leave on=false, just like a valid OFF.
         * Do not mistake that for a valid duplicate of an executed command. */
        if (c->error != KZ_OK && previous->outcome != KZ_REJECTED) return result;
        if (kz_same_command(previous, c)) {
            result.ack = previous;
            result.report_state = true;
        }
        return result;
    }
    kz_error error = c->error;
    if (error == KZ_OK && !kz_budget_valid(budget, esp_timer_get_time())) error = KZ_TIMEOUT;
    int slot = journal_reserve(c, now, error);
    /* No nondurable terminal rejection: a later retry must not regress it. */
    if (slot < 0) return result;
    if (error == KZ_OK) {
        if (!kz_budget_valid(budget, esp_timer_get_time())) {
            if (journal_abort(slot, KZ_TIMEOUT)) result.ack = journal_get(slot);
            return result;
        }
        bool applied = relay_apply_permitted(c->on, budget);
        result.report_state = true;
        if (!journal_finish(slot, applied)) return result;
    }
    result.ack = journal_get(slot);
    return result;
}
