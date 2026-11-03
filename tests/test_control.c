#include "control.h"
#include "command_engine.h"
#include "journal.h"
#include "fake_platform.h"
#include "relay.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char candidate[] = "{\"protocol_version\":\"v1\",\"execution_profile\":\"deadline-permit-v1\","
    "\"core_epoch\":\"11111111-1111-4111-8111-111111111111\","
    "\"command_id\":\"22222222-2222-4222-8222-222222222222\","
    "\"correlation_id\":\"33333333-3333-4333-8333-333333333333\","
    "\"timestamp\":\"2026-11-02T10:00:00.123456+00:00\","
    "\"expires_at\":\"2026-11-02T10:00:30.123456+00:00\",\"state\":{\"on\":true}}";
static char raw[4097], reply[2048];
static kz_control state;
static int64_t wall = 1793613601000LL;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL control line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void start(void)
{
    fake_reset_storage(); assert(journal_init() == ESP_OK);
    fake_mono_ms = 1000; control_init(&state); control_session(&state, 1, wall);
    strcpy(raw, candidate);
    assert(control_candidate(&state, raw, strlen(raw), 1, wall, fake_mono_ms*1000));
}
static void grant(const char *nonce, const char *remaining, const char *extra)
{
    snprintf(reply, sizeof(reply), "{\"control_version\":1,\"type\":\"permit_grant\","
        "\"execution_profile\":\"deadline-permit-v1\",\"core_epoch\":\"%s\",\"command_id\":\"%s\","
        "\"boot_id\":\"%s\",\"session_id\":\"%s\",\"nonce\":\"%s\",\"intent_sha256\":\"%s\","
        "\"core_time\":\"2026-11-02T10:00:01+00:00\",\"expires_at\":\"2026-11-02T10:00:30.123456+00:00\","
        "\"remaining_us\":%s%s}", state.command.epoch, state.command.command_id,
        state.boot, state.session, nonce, state.command.digest, remaining, extra);
}
static bool admitted(void *unused) { (void)unused; return fake_admitted; }
static kz_budget bounds(void)
{ return (kz_budget){.qualified=true,.core_ppm=1000,.device_ppm=1000,.epsilon_us=2000,.gpio_us=1000,.admitted=admitted}; }
int main(void)
{
    char hash[65]; CHECK(control_hash("abc",3,hash));
    CHECK(!strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    start();
    CHECK(state.command.expires_us-state.command.created_us == 30000000);
    CHECK(state.command.created_us % 1000000 == 123456);
    CHECK(control_request(&state, reply, sizeof(reply)));
    printf("CANDIDATE\t%s\nREQUEST\t%s\n", candidate, reply);
    int64_t original = state.start_us;
    strcpy(raw,candidate); CHECK(!control_candidate(&state,raw,strlen(raw),1,wall,original+100));
    CHECK(state.start_us == original);
    const char *bad_remaining[] = {"0","-1","2000001","true","1.0","1e6","null","\"100\""};
    for (unsigned i=0;i<sizeof(bad_remaining)/sizeof(bad_remaining[0]);++i) {
        grant(state.nonce,bad_remaining[i],""); kz_permit p;
        CHECK(!kz_parse_permit(reply,strlen(reply),&p));
    }
    grant(state.nonce,"1000000",",\"nonce\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"");
    kz_permit p; CHECK(!kz_parse_permit(reply,strlen(reply),&p));
    grant("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","1000000","");
    kz_budget b=bounds(); CHECK(!control_reply(&state,reply,strlen(reply),1,original+100,&b));
    CHECK(fake_apply_count==0);
    const char *bindings[]={"core_epoch","command_id","boot_id","session_id","intent_sha256"};
    for (unsigned i=0;i<sizeof(bindings)/sizeof(bindings[0]);++i) {
        grant(state.nonce,"1000000","");
        char key[64]; snprintf(key,sizeof(key),"\"%s\":\"",bindings[i]);
        char *at=strstr(reply,key); assert(at); at+=strlen(key); *at=*at=='a'?'b':'a';
        CHECK(!control_reply(&state,reply,strlen(reply),1,original+100,&b));
        CHECK(state.waiting && !fake_apply_count);
    }
    /* Malformed candidate/profile/state/deadline cannot become a request. */
    const char *fields[] = {"expires_at", "execution_profile", "command_id"};
    for (unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
        strcpy(raw,candidate); char *at=strstr(raw,fields[i]); assert(at); *at='X';
        kz_command rejected;
        CHECK(!kz_parse_control(raw,strlen(raw),state.command.digest,&rejected));
    }
    strcpy(raw,candidate); char *on=strstr(raw,"true"); memcpy(on,"null",4);
    kz_command rejected; CHECK(!kz_parse_control(raw,strlen(raw),state.command.digest,&rejected));
    grant(state.nonce,"1000000","");
    CHECK(control_reply(&state,reply,strlen(reply),1,original+100,&b));
    CHECK(!state.waiting);
    kz_result r=kz_process_permit(&state.command,wall,&b);
    CHECK(r.ack && r.ack->outcome==KZ_APPLIED && fake_apply_count==1);
    CHECK(relay_state());
    grant(state.nonce,"1000000","");
    CHECK(!control_reply(&state,reply,strlen(reply),1,original+200,&b));
    r=kz_process_permit(&state.command,wall,&b); CHECK(r.ack && fake_apply_count==1);
    fake_reboot(); CHECK(journal_init()==ESP_OK);
    r=kz_process_permit(&state.command,wall,&b); CHECK(r.ack && !relay_state() && fake_apply_count==0);
    kz_control rebooted; control_init(&rebooted); CHECK(strcmp(rebooted.boot,state.boot));
    state.command.digest[0] = state.command.digest[0]=='a'?'b':'a';
    r=kz_process_permit(&state.command,wall,&b); CHECK(!r.ack && fake_apply_count==0);
    start(); grant(state.nonce,"1000000",""); b=bounds();
    CHECK(control_reply(&state,reply,strlen(reply),1,state.start_us+100,&b));
    fake_commit_delay_ms=3000;
    r=kz_process_permit(&state.command,wall,&b);
    CHECK(r.ack && r.ack->outcome==KZ_REJECTED && fake_apply_count==0);
    start(); grant(state.nonce,"1000000",""); b=bounds();
    CHECK(control_reply(&state,reply,strlen(reply),1,state.start_us+100,&b));
    fake_revoke_on_commit=true;
    r=kz_process_permit(&state.command,wall,&b); CHECK(r.ack && fake_apply_count==0);
    start(); grant(state.nonce,"1000000",""); b=bounds(); b.qualified=false;
    CHECK(control_reply(&state,reply,strlen(reply),1,state.start_us+100,&b));
    r=kz_process_permit(&state.command,wall,&b); CHECK(r.ack && fake_apply_count==0);
    start(); grant(state.nonce,"1000000",""); b=bounds();
    CHECK(!control_reply(&state,reply,strlen(reply),2,state.start_us+100,&b));
    CHECK(!control_reply(&state,reply,strlen(reply),1,state.start_us+2000000,&b));
    control_cancel(&state,wall); CHECK(!state.waiting && fake_apply_count==0);
    start(); char session[33]; strcpy(session,state.session);
    control_session(&state,2,wall); CHECK(strcmp(session,state.session) && !state.waiting);
    grant(state.nonce,"1000000",""); CHECK(!control_reply(&state,reply,strlen(reply),2,state.start_us,&b));
    b=bounds(); b.start_us=0; b.remaining_us=1000000;
    CHECK(!kz_budget_valid(&b,-1)); CHECK(!kz_budget_valid(&b,1000000)); CHECK(kz_budget_valid(&b,900000));
    b.core_ppm=b.device_ppm=b.epsilon_us=b.gpio_us=0;
    CHECK(kz_budget_valid(&b,999999)); CHECK(!kz_budget_valid(&b,1000000));
    /* NVS v1 pending tombstone: prefix ABI upgrade, never replay/erase. */
    typedef struct { uint32_t version,outcome,error; int64_t first_ms,timestamp_ms;
        char command_id[129],correlation_id[129]; uint8_t on; } old_record;
    old_record old={.version=1,.outcome=KZ_PENDING,.first_ms=wall,.timestamp_ms=wall,.on=1};
    strcpy(old.command_id,"legacy-id"); strcpy(old.correlation_id,"legacy-correlation");
    fake_reset_storage(); fake_store_blob(0,&old,sizeof(old));
    CHECK(journal_init()==ESP_OK);
    const kz_record *loaded=journal_find("legacy-id");
    CHECK(loaded && loaded->version==2 && loaded->outcome==KZ_FAILED && !loaded->expires_us);
    CHECK(fake_apply_count==0);
    start(); grant(state.nonce,"1000000",""); b=bounds();
    CHECK(control_reply(&state,reply,strlen(reply),1,state.start_us+100,&b));
    fake_fail_commit=1;
    r=kz_process_permit(&state.command,wall,&b);
    CHECK(!r.ack && fake_apply_count==0 && !journal_healthy());
    start(); grant(state.nonce,"1000000",""); b=bounds();
    CHECK(control_reply(&state,reply,strlen(reply),1,state.start_us+100,&b));
    fake_fail_commit=2;
    r=kz_process_permit(&state.command,wall,&b); CHECK(!r.ack && fake_apply_count==1);
    fake_reboot(); CHECK(journal_init()==ESP_OK);
    r=kz_process_permit(&state.command,wall,&b); CHECK(r.ack && r.ack->outcome==KZ_FAILED && !fake_apply_count);
    start(); grant(state.nonce,"1000000",""); b=bounds();
    CHECK(control_reply(&state,reply,strlen(reply),1,state.start_us+100,&b));
    fake_gpio_fails=true;
    r=kz_process_permit(&state.command,wall,&b);
    CHECK(r.ack && r.ack->outcome==KZ_FAILED && !relay_state());
    /* Valid OFF has the same strict boolean contract and one durable write. */
    start(); control_cancel(&state,wall); fake_reset_storage(); CHECK(journal_init()==ESP_OK);
    strcpy(raw,candidate); char *yes=strstr(raw,"true");
    memmove(yes+5,yes+4,strlen(yes+4)+1); memcpy(yes,"false",5);
    CHECK(control_candidate(&state,raw,strlen(raw),1,wall,fake_mono_ms*1000));
    grant(state.nonce,"1000000",""); b=bounds();
    CHECK(control_reply(&state,reply,strlen(reply),1,state.start_us+100,&b));
    r=kz_process_permit(&state.command,wall,&b);
    CHECK(r.ack && r.ack->outcome==KZ_APPLIED && fake_apply_count==1 && !relay_state());
    /* Full journal applies backpressure; no eviction before both 600s clocks. */
    start(); b=bounds(); b.qualified=false;
    for (unsigned i=0;i<KZ_JOURNAL_SLOTS;++i) {
        snprintf(state.command.command_id,sizeof(state.command.command_id),"22222222-2222-4222-8222-%012u",i);
        r=kz_process_permit(&state.command,wall,&b); CHECK(r.ack && r.ack->outcome==KZ_REJECTED);
    }
    strcpy(state.command.command_id,"22222222-2222-4222-8222-999999999999");
    r=kz_process_permit(&state.command,wall,&b); CHECK(!r.ack && !fake_apply_count);
    fake_mono_ms+=600000;
    r=kz_process_permit(&state.command,wall,&b); CHECK(!r.ack);
    r=kz_process_permit(&state.command,wall+600000,&b); CHECK(r.ack && !fake_apply_count);
    kz_record corrupt=*r.ack; corrupt.version=99;
    fake_store_blob(0,&corrupt,sizeof(corrupt)); fake_reboot();
    CHECK(journal_init()!=ESP_OK && !journal_healthy() && !fake_apply_count);
    /* Exercise both new parsers with bounded malformed byte strings. */
    unsigned rng=11;
    for (unsigned i=0;i<20000;++i) {
        size_t n=i%511;
        for (size_t j=0;j<n;++j) { rng=rng*1664525u+1013904223u; raw[j]=(char)(rng>>24); }
        char other[512]; memcpy(other,raw,n);
        CHECK(!kz_parse_control(raw,n,hash,&rejected));
        CHECK(!kz_parse_permit(other,n,&p));
    }
    printf("PASS: %u control checks (strict parse, nonce/session, budget, NVS/reboot, delayed commit)\n",checks);
    return 0;
}
