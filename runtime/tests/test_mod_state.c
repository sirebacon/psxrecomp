/*
 * Plugin state blocks in snapshots (mod_state.c): a state written with one set
 * of mods must load cleanly with another -- each block restored, reported
 * absent or reported as a mismatch on its own, and blocks of a missing mod
 * kept for the next save.
 *
 * Build: see mod_state_test in runtime/CMakeLists.txt.
 */
#include "mod_state.h"
#include "mod_plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++s_failures;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what);  \
        }                                                                   \
    } while (0)

typedef struct { uint32_t junk; uint32_t pity[3]; } LootV1;
typedef struct { uint32_t junk; uint32_t pity[3]; uint32_t chips; } LootV2;
typedef struct { uint16_t gauge; uint16_t flags; } AtbBlock;

static LootV1 s_loot;
static LootV2 s_loot2;
static AtbBlock s_atb;

static int s_loot_result = -1, s_atb_result = -1;
static uint32_t s_loot_saved_version, s_loot_saved_size;
static uint32_t s_loot_saved_junk;
static int s_restore_calls, s_restore_saw_loot_result = -1;

static void on_loot(int result, uint32_t version, const void* saved, uint32_t size) {
    s_loot_result = result;
    s_loot_saved_version = version;
    s_loot_saved_size = size;
    s_loot_saved_junk = saved ? ((const LootV1*)saved)->junk : 0;
}
static void on_atb(int result, uint32_t version, const void* saved, uint32_t size) {
    (void)version; (void)saved; (void)size;
    s_atb_result = result;
}
static void on_restore(void) {
    ++s_restore_calls;
    s_restore_saw_loot_result = s_loot_result;
}

static void reset_results(void) {
    s_loot_result = s_atb_result = -1;
    s_loot_saved_version = s_loot_saved_size = s_loot_saved_junk = 0;
    s_restore_calls = 0;
    s_restore_saw_loot_result = -1;
}

/* Snapshot = serialize; load = the protocol boot_state drives. */
static void load(const uint8_t* section, size_t len, int restored) {
    reset_results();
    mod_state_load_begin();
    if (section) mod_state_load_section(section, len);
    mod_state_load_finish(restored);
}

static int payload_has_id(const uint8_t* p, size_t len, const char* id) {
    const size_t n = strlen(id);
    for (size_t i = 0; i + n <= len; ++i)
        if (memcmp(p + i, id, n) == 0) return 1;
    return 0;
}

static void test_registration_rules(void) {
    static uint8_t big[64u * 1024u + 1u];
    char long_id[80];
    memset(long_id, 'x', sizeof long_id - 1);
    long_id[sizeof long_id - 1] = '\0';
    mod_state_reset_for_test();
    CHECK(psx_mod_register_state("pe.loot", 1, &s_loot, sizeof s_loot, on_loot) == 1, "register");
    CHECK(psx_mod_register_state("pe.loot", 1, &s_loot, sizeof s_loot, on_loot) == 0, "duplicate id refused");
    CHECK(psx_mod_register_state("", 1, &s_loot, sizeof s_loot, NULL) == 0, "empty id refused");
    CHECK(psx_mod_register_state(long_id, 1, &s_loot, sizeof s_loot, NULL) == 0, "long id refused");
    CHECK(psx_mod_register_state("pe.x", 1, NULL, 4, NULL) == 0, "null data refused");
    CHECK(psx_mod_register_state("pe.x", 1, &s_loot, 0, NULL) == 0, "empty block refused");
    CHECK(psx_mod_register_state("pe.big", 1, big, sizeof big, NULL) == 0, "oversize block refused");
    CHECK(psx_mod_register_restore_callback(NULL) == 0, "null restore callback refused");
}

static void test_nothing_registered_writes_nothing(void) {
    uint8_t* p = (uint8_t*)1;
    size_t len = 99;
    mod_state_reset_for_test();
    CHECK(mod_state_serialize(&p, &len) == 1 && p == NULL && len == 0,
          "no blocks -> no section");
}

static void test_round_trip_and_mixed_mods(void) {
    uint8_t *snap = NULL, *snap2 = NULL;
    size_t len = 0, len2 = 0;

    /* Build 1: loot + atb. Save with junk 50. */
    mod_state_reset_for_test();
    psx_mod_register_state("pe.loot", 1, &s_loot, sizeof s_loot, on_loot);
    psx_mod_register_state("pe.atb", 1, &s_atb, sizeof s_atb, on_atb);
    psx_mod_register_restore_callback(on_restore);
    s_loot.junk = 50; s_loot.pity[1] = 7; s_atb.gauge = 4500;
    CHECK(mod_state_serialize(&snap, &len) == 1 && len > 0, "serialize");

    /* Play on, then load: everything rolls back. */
    s_loot.junk = 80; s_atb.gauge = 9000;
    load(snap, len, 1);
    CHECK(s_loot.junk == 50 && s_loot.pity[1] == 7, "loot restored");
    CHECK(s_atb.gauge == 4500, "atb restored");
    CHECK(s_loot_result == PSX_MOD_STATE_RESTORED && s_atb_result == PSX_MOD_STATE_RESTORED,
          "both report restored");
    CHECK(s_restore_calls == 1 && s_restore_saw_loot_result == PSX_MOD_STATE_RESTORED,
          "restore callback runs once, after on_loaded");

    /* A refused load changes nothing and tells nobody. */
    s_loot.junk = 81;
    load(snap, len, 0);
    CHECK(s_loot.junk == 81 && s_loot_result == -1 && s_restore_calls == 0,
          "refused load leaves plugin state alone");

    /* A state from before the mods existed: absent, data untouched. */
    load(NULL, 0, 1);
    CHECK(s_loot_result == PSX_MOD_STATE_ABSENT && s_atb_result == PSX_MOD_STATE_ABSENT,
          "no section -> absent");
    CHECK(s_loot.junk == 81, "absent leaves the block for the plugin to reset");

    /* Build 2: atb only (loot mod missing). Loading the build-1 state restores
     * atb and carries loot; the next save still holds loot. */
    mod_state_reset_for_test();
    psx_mod_register_state("pe.atb", 1, &s_atb, sizeof s_atb, on_atb);
    s_atb.gauge = 1;
    load(snap, len, 1);
    CHECK(s_atb.gauge == 4500 && s_atb_result == PSX_MOD_STATE_RESTORED,
          "present mod restored next to a missing one");
    CHECK(mod_state_serialize(&snap2, &len2) == 1 && payload_has_id(snap2, len2, "pe.loot"),
          "missing mod's block carried into the next save");

    /* Build 3: loot back, at a new layout version. The carried v1 block
     * reaches the plugin as a mismatch with the old bytes. */
    mod_state_reset_for_test();
    psx_mod_register_state("pe.loot", 2, &s_loot2, sizeof s_loot2, on_loot);
    psx_mod_register_state("pe.atb", 1, &s_atb, sizeof s_atb, on_atb);
    memset(&s_loot2, 0, sizeof s_loot2);
    load(snap2, len2, 1);
    CHECK(s_loot_result == PSX_MOD_STATE_MISMATCH, "new version -> mismatch");
    CHECK(s_loot_saved_version == 1 && s_loot_saved_size == sizeof(LootV1) &&
          s_loot_saved_junk == 50, "mismatch passes the old bytes for migration");
    CHECK(s_loot2.junk == 0, "mismatch does not copy into the new layout");
    CHECK(s_atb_result == PSX_MOD_STATE_RESTORED, "other mod unaffected");

    /* Loading a state with no section drops the carry. */
    mod_state_reset_for_test();
    psx_mod_register_state("pe.atb", 1, &s_atb, sizeof s_atb, on_atb);
    load(snap, len, 1);                 /* carries pe.loot */
    load(NULL, 0, 1);                   /* that state had no loot */
    free(snap2); snap2 = NULL;
    CHECK(mod_state_serialize(&snap2, &len2) == 1 && !payload_has_id(snap2, len2, "pe.loot"),
          "carry follows the loaded state");

    free(snap);
    free(snap2);
}

static void test_malformed_section(void) {
    uint8_t *snap = NULL;
    size_t len = 0;
    mod_state_reset_for_test();
    psx_mod_register_state("pe.loot", 1, &s_loot, sizeof s_loot, on_loot);
    s_loot.junk = 50;
    mod_state_serialize(&snap, &len);
    s_loot.junk = 80;

    load(snap, len - 3, 1);             /* truncated */
    CHECK(s_loot_result == PSX_MOD_STATE_ABSENT && s_loot.junk == 80,
          "truncated section reads as absent");
    snap[0] ^= 0xFF;                    /* bad magic */
    load(snap, len, 1);
    CHECK(s_loot_result == PSX_MOD_STATE_ABSENT && s_loot.junk == 80,
          "bad magic reads as absent");
    free(snap);
}

int main(void) {
    test_registration_rules();
    test_nothing_registered_writes_nothing();
    test_round_trip_and_mixed_mods();
    test_malformed_section();
    mod_state_reset_for_test();
    if (s_failures) {
        fprintf(stderr, "mod_state_test: %d failure(s)\n", s_failures);
        return 1;
    }
    puts("mod_state_test: all checks passed");
    return 0;
}
