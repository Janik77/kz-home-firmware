#include "command_engine.h"
#include "journal.h"
#include "relay.h"

kz_result kz_process(const kz_command *c, int64_t now, bool retained)
{
    kz_result result = {0};
    if (!c->identifiable || now <= 0 || !journal_healthy()) return result;
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
    kz_error error = retained ? KZ_PROTOCOL : c->error;
    if (error == KZ_OK) error = kz_freshness(c->timestamp_ms, now);
    int slot = journal_reserve(c, now, error);
    /* No nondurable terminal rejection: a later retry must not regress it. */
    if (slot < 0) return result;
    if (error == KZ_OK) {
        bool applied = relay_apply(c->on);
        result.report_state = true;
        if (!journal_finish(slot, applied)) return result;
    }
    result.ack = journal_get(slot);
    return result;
}
