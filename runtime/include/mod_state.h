#ifndef PSX_MOD_STATE_H
#define PSX_MOD_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host side of psx_mod_register_state() (mod_plugins.h): carries plugin state
 * through whole-machine snapshots -- F7 save states, rewind, netplay rollback
 * and the fast-boot snapshot all go through boot_state, which calls these.
 *
 * Blocks travel by id, never by address, so a state written with a different
 * set of mods loads cleanly: each registered block is restored, reported
 * absent, or reported as a version/size mismatch on its own. Blocks in a
 * loaded state that no running plugin owns are kept and written into the next
 * snapshot, so playing a while with a mod missing does not erase its data.
 */

/* Serialize every registered block plus the carried ones into a malloc'd
 * payload. Returns 1 with *out_len = 0 (and *out = NULL) when there is
 * nothing to save, 0 on allocation failure. */
int  mod_state_serialize(uint8_t** out, size_t* out_len);

/* Load protocol, driven by boot_state_load_buffer: begin, at most one
 * section, then finish(1) after the whole machine was restored or finish(0)
 * when the load was refused. A malformed section is discarded (every block
 * then reads as absent); it never fails the machine load. */
void mod_state_load_begin(void);
void mod_state_load_section(const uint8_t* payload, size_t len);
void mod_state_load_finish(int machine_restored);

/* Test hook: forget every registration and carried block. */
void mod_state_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_MOD_STATE_H */
