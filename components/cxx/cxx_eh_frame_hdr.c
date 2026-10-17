/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * libgcc in the ESP toolchains is built without FDE sorting, so its _Unwind_Find_FDE searches all of
 * .eh_frame linearly for every frame it unwinds. The linker's --eh-frame-hdr table is sorted by PC:
 * frames are looked up there by binary search instead, and a PC the table doesn't cover is left to
 * libgcc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* libgcc's struct dwarf_eh_bases (unwind-dw2-fde.h) */
struct dwarf_eh_bases {
    void *tbase;
    void *dbase;
    void *func;
};

/* DW_EH_PE_* pointer encodings */
#define ENCODING_ABSPTR     0x00
#define ENCODING_UDATA4     0x03
#define ENCODING_SDATA4     0x0b
#define ENCODING_DATAREL    0x30
#define ENCODING_ALIGNED    0x50

typedef struct {
    uint8_t version;
    uint8_t eh_frame_ptr_enc;
    uint8_t fde_count_enc;
    uint8_t table_enc;
    int32_t eh_frame_ptr;
    uint32_t fde_count;
} eh_frame_hdr_t;
_Static_assert(sizeof(eh_frame_hdr_t) == 12, "unexpected .eh_frame_hdr header size");

typedef struct {
    int32_t initial_location;
    int32_t fde;
} eh_frame_hdr_entry_t;
_Static_assert(sizeof(eh_frame_hdr_entry_t) == 8, "unexpected .eh_frame_hdr table entry size");

extern const uint8_t __eh_frame_hdr_start[];
extern const uint8_t __eh_frame_hdr_end[];
extern const uint8_t __eh_frame[];
extern const uint8_t __eh_frame_end[];

const void *__real__Unwind_Find_FDE(void *pc, struct dwarf_eh_bases *bases);
const void *__wrap__Unwind_Find_FDE(void *pc, struct dwarf_eh_bases *bases);

/* The size of a value in `encoding`, or 0 for the variable-length ones */
static size_t encoded_size(uint8_t encoding)
{
    switch (encoding & 0x0f) {
    case 0x00:
        return sizeof(void *);
    case 0x02:
    case 0x0a:
        return 2;
    case 0x03:
    case 0x0b:
        return 4;
    case 0x04:
    case 0x0c:
        return 8;
    default:
        return 0;
    }
}

static bool in_eh_frame(const uint8_t *p, size_t size)
{
    return p >= __eh_frame && p <= __eh_frame_end && (size_t)(__eh_frame_end - p) >= size;
}

static const uint8_t *skip_leb128(const uint8_t *p)
{
    while (in_eh_frame(p, 1)) {
        if ((*p++ & 0x80) == 0) {
            return p;
        }
    }
    return NULL;
}

/* The pointer encoding of the FDEs under `cie`: its 'R' augmentation, absptr without one. Returns false if unsupported. */
static bool fde_pointer_encoding(const uint8_t *cie, uint8_t *out_encoding)
{
    if (!in_eh_frame(cie, 9)) {
        return false;
    }
    uint8_t version = cie[8];
    const char *augmentation = (const char *)(cie + 9);
    size_t augmentation_len = strnlen(augmentation, (size_t)(__eh_frame_end - (const uint8_t *)augmentation));
    if (!in_eh_frame((const uint8_t *)augmentation, augmentation_len + 1)) {
        return false;
    }
    if (augmentation_len == 0) {
        *out_encoding = ENCODING_ABSPTR;
        return true;
    }
    if (augmentation[0] != 'z') {
        return false;
    }

    /* Code alignment, data alignment, return address register, augmentation data length */
    const uint8_t *p = (const uint8_t *)augmentation + augmentation_len + 1;
    p = skip_leb128(p);
    p = p ? skip_leb128(p) : NULL;
    p = p ? (version == 1 ? p + 1 : skip_leb128(p)) : NULL;
    p = p ? skip_leb128(p) : NULL;
    for (size_t i = 1; i < augmentation_len; i++) {
        if (!p || !in_eh_frame(p, 1)) {
            return false;
        }
        switch (augmentation[i]) {
        case 'R':
            *out_encoding = *p;
            return true;
        case 'P': {
            uint8_t encoding = *p;
            size_t size = encoded_size(encoding);
            if ((encoding & 0x70) == ENCODING_ALIGNED || size == 0) {
                return false;
            }
            p += 1 + size;
            break;
        }
        case 'L':
            p += 1;
            break;
        case 'S':
            break;
        default:
            return false;
        }
    }
    *out_encoding = ENCODING_ABSPTR;
    return true;
}

static const void *find_fde(void *pc, struct dwarf_eh_bases *bases)
{
    const uint8_t *hdr_start = __eh_frame_hdr_start;
    size_t hdr_size = (size_t)(__eh_frame_hdr_end - __eh_frame_hdr_start);
    eh_frame_hdr_t hdr;
    if (hdr_size < sizeof(hdr)) {
        return NULL;
    }
    memcpy(&hdr, hdr_start, sizeof(hdr));
    if (hdr.version != 1
            || hdr.fde_count_enc != ENCODING_UDATA4
            || hdr.table_enc != (ENCODING_DATAREL | ENCODING_SDATA4)
            || (hdr_size - sizeof(hdr)) / sizeof(eh_frame_hdr_entry_t) < hdr.fde_count) {
        return NULL;
    }

    /* Table entries are relative to the start of .eh_frame_hdr; find the last one at or before pc */
    const uint8_t *table = hdr_start + sizeof(hdr);
    int32_t target = (int32_t)((uintptr_t)pc - (uintptr_t)hdr_start);
    uint32_t low = 0;
    uint32_t high = hdr.fde_count;
    eh_frame_hdr_entry_t entry;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        memcpy(&entry, table + mid * sizeof(entry), sizeof(entry));
        if (entry.initial_location <= target) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low == 0) {
        return NULL;
    }
    memcpy(&entry, table + (low - 1) * sizeof(entry), sizeof(entry));

    /* length, CIE pointer (back from itself to the CIE), pc_begin, pc_range */
    const uint8_t *fde = hdr_start + entry.fde;
    uint32_t cie_pointer;
    if (!in_eh_frame(fde, 8)) {
        return NULL;
    }
    memcpy(&cie_pointer, fde + 4, sizeof(cie_pointer));
    if (cie_pointer > (size_t)(fde + 4 - __eh_frame)) {
        return NULL;
    }
    uint8_t encoding;
    if (!fde_pointer_encoding(fde + 4 - cie_pointer, &encoding)) {
        return NULL;
    }
    size_t size = encoded_size(encoding);
    if (size == 0 || !in_eh_frame(fde + 8, 2 * size)) {
        return NULL;
    }
    uint64_t range = 0;
    for (size_t i = 0; i < size; i++) {
        range |= (uint64_t)fde[8 + size + i] << (8 * i);
    }

    uintptr_t func = (uintptr_t)hdr_start + entry.initial_location;
    if ((uintptr_t)pc - func >= range) {
        return NULL;
    }

    /* .eh_frame is registered with no text or data base (__register_frame_info in startup.c) */
    bases->tbase = NULL;
    bases->dbase = NULL;
    bases->func = (void *)func;
    return fde;
}

const void *__wrap__Unwind_Find_FDE(void *pc, struct dwarf_eh_bases *bases)
{
    const void *fde = find_fde(pc, bases);
    if (fde) {
        return fde;
    }
    return __real__Unwind_Find_FDE(pc, bases);
}
