/*
 * cdl-table.h - open-addressing hash tables with a dirty list, used by cdl.c
 * for xrefs, edges and per-PC stats. Keys are 64-bit with bit 63 set (0 = empty);
 * every insert or value change is appended once to the dirty list so a drain
 * returns only what changed.
 */

#ifndef CDL_TABLE_H
#define CDL_TABLE_H

#include "cdl.h"

#if EVS_CDL

typedef struct {
    uint64_t* keys;     /* 0 = empty */
    uint8_t*  vals;
    uint8_t*  dirty;
    uint16_t* count;    /* stats tables only */
    uint32_t* lo;
    uint32_t* hi;
    uint32_t  cap, used;
    uint32_t* dlist;
    uint32_t  dcount, dcap;
    bool      stats;
} Table;

static inline uint32_t hash64(uint64_t k)
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

void     tableFree(Table* t);
bool     tableAlloc(Table* t, uint32_t cap, bool withStats);
void     markDirty(Table* t, uint32_t i);
/* slot for key, inserting it when allowed; UINT32_MAX when absent or full */
uint32_t tableSlot(Table* t, uint64_t key, bool insert);
void     tableOr(Table* t, uint64_t key, uint8_t flags);

#endif /* EVS_CDL */

#endif /* CDL_TABLE_H */
