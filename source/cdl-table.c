/*
 * cdl-table.c - hash tables for the CDL recorder (see cdl-table.h)
 */

#include <stdlib.h>
#include <string.h>
#include "cdl-table.h"

#if EVS_CDL

#define TABLE_MAX (1u << 23)

void tableFree(Table* t)
{
    free(t->keys); free(t->vals); free(t->dirty); free(t->count);
    free(t->lo); free(t->hi); free(t->dlist);
    memset(t, 0, sizeof(*t));
}

bool tableAlloc(Table* t, uint32_t cap, bool withStats)
{
    memset(t, 0, sizeof(*t));
    t->cap   = cap;
    t->stats = withStats;
    t->keys  = calloc(cap, sizeof(uint64_t));
    t->vals  = calloc(cap, 1);
    t->dirty = calloc(cap, 1);
    if (withStats) {
        t->count = calloc(cap, sizeof(uint16_t));
        t->lo    = calloc(cap, sizeof(uint32_t));
        t->hi    = calloc(cap, sizeof(uint32_t));
    }
    t->dcap  = 1024;
    t->dlist = malloc(t->dcap * sizeof(uint32_t));
    return t->keys && t->vals && t->dirty && t->dlist && (!withStats || (t->count && t->lo && t->hi));
}

void markDirty(Table* t, uint32_t i)
{
    if (t->dirty[i]) return;
    t->dirty[i] = 1;
    if (t->dcount == t->dcap) {
        uint32_t* grown = realloc(t->dlist, t->dcap * 2 * sizeof(uint32_t));
        if (!grown) return;
        t->dlist = grown;
        t->dcap *= 2;
    }
    t->dlist[t->dcount++] = i;
}

static uint32_t findSlot(const Table* t, uint64_t key)
{
    uint32_t mask = t->cap - 1;
    uint32_t i = hash64(key) & mask;
    while (t->keys[i] && t->keys[i] != key) i = (i + 1) & mask;
    return i;
}

static bool tableGrow(Table* t)
{
    Table n;
    uint32_t i;
    if (t->cap >= TABLE_MAX) return false;
    if (!tableAlloc(&n, t->cap * 2, t->stats)) { tableFree(&n); return false; }
    for (i = 0; i < t->cap; i++) {
        uint32_t j;
        if (!t->keys[i]) continue;
        j = findSlot(&n, t->keys[i]);
        n.keys[j] = t->keys[i];
        n.vals[j] = t->vals[i];
        if (t->stats) { n.count[j] = t->count[i]; n.lo[j] = t->lo[i]; n.hi[j] = t->hi[i]; }
        if (t->dirty[i]) markDirty(&n, j);
    }
    n.used = t->used;
    tableFree(t);
    *t = n;
    return true;
}

/* Returns the slot for key, inserting it when allowed; UINT32_MAX when absent/full. */
uint32_t tableSlot(Table* t, uint64_t key, bool insert)
{
    uint32_t i;
    if (!t->keys) return UINT32_MAX;
    i = findSlot(t, key);
    if (t->keys[i]) return i;
    if (!insert) return UINT32_MAX;
    if ((t->used + 1) * 2 > t->cap) {
        if (!tableGrow(t)) return UINT32_MAX;
        i = findSlot(t, key);
    }
    t->keys[i] = key;
    t->used++;
    markDirty(t, i);
    return i;
}

void tableOr(Table* t, uint64_t key, uint8_t flags)
{
    uint32_t i = tableSlot(t, key, true);
    if (i == UINT32_MAX) return;
    if ((t->vals[i] | flags) != t->vals[i]) { t->vals[i] |= flags; markDirty(t, i); }
}

#endif /* EVS_CDL */
