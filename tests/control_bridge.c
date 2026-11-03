/* Pipe transport for real Core authority -> production firmware C integration. */
#include "control.h"
#include "journal.h"
#include "command_engine.h"
#include "fake_platform.h"
#include "relay.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
static bool admitted(void *unused) { (void)unused; return fake_admitted; }
int main(int argc, char **argv)
{
    char command[4098], raw[4098], request[1000]; kz_control state;
    fake_reset_storage(); assert(journal_init()==ESP_OK); fake_mono_ms=1000;
    control_init(&state); control_session(&state,1,1793613601000LL);
    assert(fgets(command,sizeof(command),stdin)); command[strcspn(command,"\r\n")]=0;
    strcpy(raw,command);
    assert(control_candidate(&state,raw,strlen(raw),1,1793613601000LL,1000000));
    assert(control_request(&state,request,sizeof(request)));
    puts(request); fflush(stdout);
    assert(fgets(raw,sizeof(raw),stdin)); raw[strcspn(raw,"\r\n")]=0;
    char duplicate[4098]; strcpy(duplicate,raw);
    if (argc>1 && !strcmp(argv[1],"late")) fake_mono_ms+=3000;
    kz_budget b={.qualified=true,.core_ppm=1000,.device_ppm=1000,.epsilon_us=2000,.gpio_us=1000,.admitted=admitted};
    kz_result result={0};
    if (control_reply(&state,raw,strlen(raw),1,fake_mono_ms*1000,&b))
        result=kz_process_permit(&state.command,1793613601000LL,&b);
    int first=fake_apply_count;
    assert(!control_reply(&state,duplicate,strlen(duplicate),1,fake_mono_ms*1000,&b));
    assert(first==fake_apply_count);
    printf("{\"writes\":%d,\"status\":\"%s\",\"on\":%s}",first,
        result.ack?kz_outcome_name(result.ack->outcome):"none",relay_state()?"true":"false");
    puts(""); return 0;
}
