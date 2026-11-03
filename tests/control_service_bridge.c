/* Interactive host-only adapter. No network credentials or physical GPIO. */
#include "control.h"
#include "journal.h"
#include "command_engine.h"
#include "fake_platform.h"
#include "relay.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static kz_control state;
static bool admitted(void *unused) { (void)unused; return fake_admitted; }
static const int64_t wall=1793613601000LL;
static void boot(void) {
    fake_reboot(); assert(journal_init()==ESP_OK);
    fake_mono_ms=1000; control_init(&state); control_session(&state,1,wall);
}
int main(int argc, char **argv) {
    bool lab=argc==2 && !strcmp(argv[1],"lab");
    char line[8200], request[1000]; unsigned generation=1;
    fake_reset_storage(); boot();
    while (fgets(line,sizeof(line),stdin)) {
        line[strcspn(line,"\r\n")]=0;
        char *payload=strchr(line,'\t'); if (payload) *payload++=0;
        kz_result result={0}; bool valid=false;
        if (!strcmp(line,"advance") && payload) fake_mono_ms+=strtoll(payload,NULL,10);
        else if (!strcmp(line,"reconnect")) control_session(&state,++generation,wall);
        else if (!strcmp(line,"reboot")) { boot(); generation=1; }
        else if (!strcmp(line,"clock-fault")) { control_cancel(&state,0); fake_admitted=false; }
        else if (!strcmp(line,"candidate") && payload) {
            valid=control_candidate(&state,payload,strlen(payload),generation,wall,fake_mono_ms*1000);
            if (valid && control_request(&state,request,sizeof(request))) {
                printf("{\"request\":%s,\"writes\":%d}\n",request,fake_apply_count); fflush(stdout); continue;
            }
            result.ack=state.duplicate;
        } else if (!strcmp(line,"reply") && payload) {
            kz_budget b={.qualified=lab,.core_ppm=1000,.device_ppm=1000,
                .epsilon_us=2000,.gpio_us=1000,.admitted=admitted};
            valid=control_reply(&state,payload,strlen(payload),generation,fake_mono_ms*1000,&b);
            if (valid) result=kz_process_permit(&state.command,wall,&b);
        } /* legacy/set and unknown operations are inert. */
        printf("{\"valid\":%s,\"waiting\":%s,\"writes\":%d,\"status\":\"%s\",\"on\":%s}\n",
            valid?"true":"false",state.waiting?"true":"false",fake_apply_count,
            result.ack?kz_outcome_name(result.ack->outcome):"none",relay_state()?"true":"false");
        fflush(stdout);
    }
    return 0;
}
