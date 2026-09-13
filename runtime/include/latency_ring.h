#ifndef PSX_LATENCY_RING_H
#define PSX_LATENCY_RING_H

/* Always-on input->photon latency ring.
 *
 * Every present cycle (one sdl_vblank_present call) stamps a QPC timestamp at
 * a few fixed stages.  The ring records continuously from process start so a
 * probe can QUERY a window of recent frames after the fact (never arm-then-
 * capture).  It is backend-agnostic: the GL and Vulkan present paths both mark
 * the same SWAP_BEGIN/SWAP_END boundary, so their latency is directly
 * comparable.  Query through the debug server ("latency" command).
 *
 * Single writer (the emu/vblank thread) marks; the debug-server IO thread reads
 * best-effort.  Timestamps are monotonic and a torn read only blurs one frame's
 * diagnostic, so no locking is used (matches the other always-on rings). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAT_INPUT = 0,    /* pad state sampled into SIO for this cycle           */
    LAT_PACED,        /* frame_pacer_wait() returned (wall-clock cap done)   */
    LAT_SWAP_BEGIN,   /* immediately before the backend present/swap         */
    LAT_SWAP_END,     /* immediately after the present/swap returned         */
    LAT_STAGE_COUNT
} LatencyStage;

/* Begin a new frame slot and stamp LAT_INPUT.  Call once per cycle, at the
 * point input is sampled.  Advances the ring head. */
void latency_ring_frame_begin(void);

/* Stamp the current frame slot at `stage` (no-op for LAT_INPUT — use
 * latency_ring_frame_begin for that). */
void latency_ring_mark(LatencyStage stage);

/* Re-stamp the current slot's LAT_INPUT to now.  Used by the low-latency
 * present path, which re-samples the pad AFTER the pacer wait: the input the
 * CPU actually reads is this later one, so input_to_swap must measure from it.
 * (With this active, input_to_paced is negative and drops out of the summary.) */
void latency_ring_restamp_input(void);

/* Note which present backend produced the most recent swaps ("opengl",
 * "software", "vulkan").  Reported in the summary so a comparison is labelled. */
void latency_ring_set_backend(const char *name);

/* Note the effective present mode / swap interval for the summary label
 * (e.g. 1 = vsync, 0 = immediate, -1 = adaptive; other backends pass their
 * own code).  Purely descriptive. */
void latency_ring_set_present_mode(int mode);

/* JSON summary over the last `window` completed frames (clamped): mean/min/max/
 * p50/p95 in microseconds of frame_period, input->swap, and swap_block (the
 * present/swap blocking time — the key vsync/queue-latency signal).  Writes a
 * brace-delimited object into buf; returns bytes written (excluding NUL). */
int latency_ring_summary_json(char *buf, int buf_size, int window);

/* JSON array of the last `max_frames` raw frame records (timestamps in
 * microseconds relative to the oldest reported frame). */
int latency_ring_dump_json(char *buf, int buf_size, int max_frames);

/* TEMPORARY diagnostic (2026-09-12, FMV-dip investigation — see
 * findings/fmv-frame-stall.md): correlate disc-image sector-read duration
 * against the frame it happened in, so a slow synchronous read (iso_reader.cpp
 * has no read-ahead/cache) can be checked directly against a frame_period
 * outlier from the same dump instead of guessed at. Call
 * latency_ring_cd_read_begin() right before the read, then
 * latency_ring_cd_read_end() right after with the same value and the LBA
 * read; the current frame slot keeps the single slowest read of the frame
 * (and how many reads happened) — enough to see, per frame, "yes a read
 * happened here and it took N microseconds" without a separate ring. Remove
 * once the WO-8 (or whatever it gets numbered) finding is settled either way. */
uint64_t latency_ring_cd_read_begin(void);
void latency_ring_cd_read_end(uint64_t start_ticks, uint32_t lba);

/* TEMPORARY diagnostic (2026-09-12 FMV-dip investigation, Phase 2 of
 * findings/fmv-frame-stall-plan.md) — same pattern as the CD-read pair
 * above, applied to the two systems FMV exercises that ordinary gameplay
 * doesn't (Phase 1 found FMV-specific stalls at ~12x gameplay's rate):
 * MDEC video decode (mdec.c's execute_decode()) and XA-ADPCM audio decode
 * (cdrom.c's maybe_deliver_xa_audio()). Same contract: begin right before
 * the call, end right after with the same start value; the current frame
 * slot keeps the single slowest call of the frame and how many happened. */
uint64_t latency_ring_mdec_begin(void);
void latency_ring_mdec_end(uint64_t start_ticks);
uint64_t latency_ring_xa_begin(void);
void latency_ring_xa_end(uint64_t start_ticks);

/* TEMPORARY diagnostic (2026-09-12, Phase 2 continued) — MDEC/XA/CD are all
 * ruled out (max ever seen well under 2ms; the single worst stall this
 * session, 163ms, had zero calls to any of them). Times the actual native
 * overlay dispatch call itself (overlay_loader.c's `c->fn(cpu)`), which
 * every earlier check only measured by RATE (calls/sec), never by
 * individual call duration — catches any one unusually slow native
 * function, not just the ones already guessed at. Records the slowest
 * call's guest address too, so a hit can be attributed. */
uint64_t latency_ring_dispatch_begin(void);
void latency_ring_dispatch_end(uint64_t start_ticks, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* PSX_LATENCY_RING_H */
