/*
 * Plugin state blocks in whole-machine snapshots (psx_mod_register_state in
 * mod_plugins.h, host protocol in mod_state.h).
 *
 * Section payload (all little-endian):
 *     u32 magic  'PMST'
 *     u32 format  1
 *     u32 count
 *     count x { u32 id_len; u8 id[id_len]; u32 version; u32 size; u8 data[size] }
 *
 * The registry is plain zero-initialized static storage, so plugin
 * constructors may register in any order before main().
 */
#include "mod_state.h"
#include "mod_plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD_STATE_MAGIC        0x54534D50u  /* "PMST" */
#define MOD_STATE_FORMAT       1u
#define MOD_STATE_MAX_ID       63u
#define MOD_STATE_MAX_SIZE     (64u * 1024u)
#define MOD_STATE_MAX_BLOCKS   32u
#define MOD_STATE_MAX_RESTORE  16u
/* A loaded state may hold blocks from mods this build lacks. */
#define MOD_STATE_MAX_SAVED    64u

typedef struct {
    char id[MOD_STATE_MAX_ID + 1];
    uint32_t version;
    void* data;
    uint32_t size;
    PSXModStateLoadedCallback on_loaded;
} RegisteredBlock;

/* A block read from a snapshot (staged during a load, or carried). */
typedef struct {
    char id[MOD_STATE_MAX_ID + 1];
    uint32_t version;
    uint32_t size;
    uint8_t* bytes;
} SavedBlock;

static RegisteredBlock s_blocks[MOD_STATE_MAX_BLOCKS];
static uint32_t s_block_count;
static PSXModRestoreCallback s_restore[MOD_STATE_MAX_RESTORE];
static uint32_t s_restore_count;

static SavedBlock s_carry[MOD_STATE_MAX_SAVED];
static uint32_t s_carry_count;
static SavedBlock s_staged[MOD_STATE_MAX_SAVED];
static uint32_t s_staged_count;

static void free_saved(SavedBlock* list, uint32_t* count) {
    for (uint32_t i = 0; i < *count; ++i) free(list[i].bytes);
    memset(list, 0, sizeof(SavedBlock) * MOD_STATE_MAX_SAVED);
    *count = 0;
}

static const RegisteredBlock* find_registered(const char* id) {
    for (uint32_t i = 0; i < s_block_count; ++i)
        if (strcmp(s_blocks[i].id, id) == 0) return &s_blocks[i];
    return NULL;
}

int psx_mod_register_state(const char* id, uint32_t version, void* data,
                           uint32_t size, PSXModStateLoadedCallback on_loaded) {
    size_t id_len;
    RegisteredBlock* b;
    if (!id || !data || size == 0 || size > MOD_STATE_MAX_SIZE) return 0;
    id_len = strlen(id);
    if (id_len == 0 || id_len > MOD_STATE_MAX_ID) return 0;
    if (find_registered(id) || s_block_count >= MOD_STATE_MAX_BLOCKS) return 0;
    b = &s_blocks[s_block_count++];
    memcpy(b->id, id, id_len + 1);
    b->version = version;
    b->data = data;
    b->size = size;
    b->on_loaded = on_loaded;
    return 1;
}

int psx_mod_register_restore_callback(PSXModRestoreCallback callback) {
    if (!callback || s_restore_count >= MOD_STATE_MAX_RESTORE) return 0;
    s_restore[s_restore_count++] = callback;
    return 1;
}

/* ---- serialize ----------------------------------------------------------- */

static uint8_t* put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static uint8_t* put_block(uint8_t* p, const char* id, uint32_t version,
                          const void* data, uint32_t size) {
    const uint32_t id_len = (uint32_t)strlen(id);
    p = put_u32(p, id_len);
    memcpy(p, id, id_len);
    p += id_len;
    p = put_u32(p, version);
    p = put_u32(p, size);
    memcpy(p, data, size);
    return p + size;
}

int mod_state_serialize(uint8_t** out, size_t* out_len) {
    size_t total = 12;
    uint32_t count = 0;
    uint8_t* buf;
    uint8_t* p;
    *out = NULL;
    *out_len = 0;
    for (uint32_t i = 0; i < s_block_count; ++i) {
        total += 12u + strlen(s_blocks[i].id) + s_blocks[i].size;
        ++count;
    }
    for (uint32_t i = 0; i < s_carry_count; ++i) {
        if (find_registered(s_carry[i].id)) continue;
        total += 12u + strlen(s_carry[i].id) + s_carry[i].size;
        ++count;
    }
    if (count == 0) return 1;
    buf = (uint8_t*)malloc(total);
    if (!buf) return 0;
    p = put_u32(buf, MOD_STATE_MAGIC);
    p = put_u32(p, MOD_STATE_FORMAT);
    p = put_u32(p, count);
    for (uint32_t i = 0; i < s_block_count; ++i)
        p = put_block(p, s_blocks[i].id, s_blocks[i].version,
                      s_blocks[i].data, s_blocks[i].size);
    for (uint32_t i = 0; i < s_carry_count; ++i) {
        if (find_registered(s_carry[i].id)) continue;
        p = put_block(p, s_carry[i].id, s_carry[i].version,
                      s_carry[i].bytes, s_carry[i].size);
    }
    *out = buf;
    *out_len = total;
    return 1;
}

/* ---- load ---------------------------------------------------------------- */

static int get_u32(const uint8_t** p, const uint8_t* end, uint32_t* v) {
    if ((size_t)(end - *p) < 4u) return 0;
    *v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
         ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return 1;
}

static int staged_has(const char* id) {
    for (uint32_t i = 0; i < s_staged_count; ++i)
        if (strcmp(s_staged[i].id, id) == 0) return 1;
    return 0;
}

void mod_state_load_begin(void) {
    free_saved(s_staged, &s_staged_count);
}

void mod_state_load_section(const uint8_t* payload, size_t len) {
    const uint8_t* p = payload;
    const uint8_t* end = payload + len;
    uint32_t magic = 0, format = 0, count = 0;

    free_saved(s_staged, &s_staged_count);
    if (!payload || !get_u32(&p, end, &magic) || !get_u32(&p, end, &format) ||
        !get_u32(&p, end, &count) || magic != MOD_STATE_MAGIC ||
        format != MOD_STATE_FORMAT) {
        fprintf(stderr, "mod_state: unreadable plugin state section ignored\n");
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id_len = 0, version = 0, size = 0;
        char id[MOD_STATE_MAX_ID + 1];
        SavedBlock* s;
        if (!get_u32(&p, end, &id_len) || id_len == 0 ||
            id_len > MOD_STATE_MAX_ID || (size_t)(end - p) < id_len)
            goto malformed;
        memcpy(id, p, id_len);
        id[id_len] = '\0';
        p += id_len;
        if (memchr(id, '\0', id_len)) goto malformed;
        if (!get_u32(&p, end, &version) || !get_u32(&p, end, &size) ||
            size == 0 || size > MOD_STATE_MAX_SIZE || (size_t)(end - p) < size)
            goto malformed;
        if (staged_has(id) || s_staged_count >= MOD_STATE_MAX_SAVED) {
            p += size;  /* duplicate or over the cap: first one wins */
            continue;
        }
        s = &s_staged[s_staged_count];
        s->bytes = (uint8_t*)malloc(size);
        if (!s->bytes) goto malformed;
        memcpy(s->bytes, p, size);
        memcpy(s->id, id, id_len + 1);
        s->version = version;
        s->size = size;
        ++s_staged_count;
        p += size;
    }
    return;

malformed:
    fprintf(stderr, "mod_state: malformed plugin state section ignored\n");
    free_saved(s_staged, &s_staged_count);
}

void mod_state_load_finish(int machine_restored) {
    int result[MOD_STATE_MAX_BLOCKS];
    const SavedBlock* match[MOD_STATE_MAX_BLOCKS];

    if (!machine_restored) {
        /* Refused load: the running session keeps its plugin state. */
        free_saved(s_staged, &s_staged_count);
        return;
    }

    /* The loaded snapshot's unowned blocks replace the old carry. */
    free_saved(s_carry, &s_carry_count);

    for (uint32_t i = 0; i < s_block_count; ++i) {
        RegisteredBlock* b = &s_blocks[i];
        match[i] = NULL;
        result[i] = PSX_MOD_STATE_ABSENT;
        for (uint32_t j = 0; j < s_staged_count; ++j) {
            if (strcmp(s_staged[j].id, b->id) != 0) continue;
            match[i] = &s_staged[j];
            if (s_staged[j].version == b->version && s_staged[j].size == b->size) {
                memcpy(b->data, s_staged[j].bytes, b->size);
                result[i] = PSX_MOD_STATE_RESTORED;
            } else {
                result[i] = PSX_MOD_STATE_MISMATCH;
            }
            break;
        }
    }

    /* Every block is in place before any plugin hears about the load. */
    for (uint32_t i = 0; i < s_block_count; ++i) {
        const RegisteredBlock* b = &s_blocks[i];
        if (!b->on_loaded) continue;
        if (result[i] == PSX_MOD_STATE_ABSENT)
            b->on_loaded(PSX_MOD_STATE_ABSENT, 0, NULL, 0);
        else
            b->on_loaded(result[i], match[i]->version, match[i]->bytes,
                         match[i]->size);
    }
    for (uint32_t i = 0; i < s_restore_count; ++i) s_restore[i]();

    /* Keep the blocks no running plugin owns; drop the rest. */
    for (uint32_t j = 0; j < s_staged_count; ++j) {
        if (find_registered(s_staged[j].id)) continue;
        s_carry[s_carry_count++] = s_staged[j];
        s_staged[j].bytes = NULL;
    }
    free_saved(s_staged, &s_staged_count);
}

void mod_state_reset_for_test(void) {
    memset(s_blocks, 0, sizeof s_blocks);
    s_block_count = 0;
    memset(s_restore, 0, sizeof s_restore);
    s_restore_count = 0;
    free_saved(s_carry, &s_carry_count);
    free_saved(s_staged, &s_staged_count);
}
