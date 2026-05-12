#include "mp_pool.h"
#include <string.h>  /* memmove, memset */

/* ═══════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════ */

/* ── Metadata accessors ──────────────────────────────────────── */
static mp_meta_header_t *hdr(const mp_pool_t *pool) {
    return (mp_meta_header_t *)pool->metadata;
}

static mp_page_desc_t *page_desc_tbl(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return (mp_page_desc_t *)((uint8_t *)pool->metadata + mh->off_page_desc);
}

static mp_handle_entry_t *handle_tbl(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return (mp_handle_entry_t *)((uint8_t *)pool->metadata + mh->off_handle_entry);
}

static uint16_t *vpn2ppn_tbl(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return (uint16_t *)((uint8_t *)pool->metadata + mh->off_vpn2ppn);
}

static uint16_t *ppn2vpn_tbl(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return (uint16_t *)((uint8_t *)pool->metadata + mh->off_ppn2vpn);
}

static mp_free_block_node_t *free_node_arr(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return (mp_free_block_node_t *)((uint8_t *)pool->metadata + mh->off_free_nodes);
}

static uint8_t *handle_bm(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return (uint8_t *)((uint8_t *)pool->metadata + mh->off_handle_bm);
}

static uint8_t *node_bm(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return (uint8_t *)((uint8_t *)pool->metadata + mh->off_node_bm);
}

/* ── Bitmap helpers ──────────────────────────────────────────── */

/** Find and allocate a free bit in a bitmap.  Returns index or 0xFFFF. */
static uint16_t bm_alloc(uint8_t *bm, uint16_t nbits) {
    for (uint16_t i = 0; i < nbits; i++) {
        if (bm[i >> 3] & (1u << (i & 7))) {
            bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
            return i;
        }
    }
    return 0xFFFF;
}

static void bm_free(uint8_t *bm, uint16_t idx) {
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

/* ── Free-block list helpers ─────────────────────────────────── */

static uint16_t fb_alloc_node(const mp_pool_t *pool) {
    mp_meta_header_t *mh = hdr(pool);
    return bm_alloc(node_bm(pool), (uint16_t)(mh->max_handles + 1));
}

static void fb_free_node(const mp_pool_t *pool, uint16_t idx) {
    bm_free(node_bm(pool), idx);
}

/** Return the previous-node index whose next == idx, or 0xFFFF. */
static uint16_t fb_find_prev(const mp_pool_t *pool, uint16_t head, uint16_t idx) {
    mp_free_block_node_t *fn = free_node_arr(pool);
    if (head == idx) return 0xFFFF;
    uint16_t cur = head;
    while (cur != 0xFFFF && fn[cur].next != idx)
        cur = fn[cur].next;
    return cur;
}

/** Insert node idx into the free list sorted by start_ppn. */
static void fb_list_insert(const mp_pool_t *pool, uint16_t idx) {
    mp_meta_header_t   *mh = hdr(pool);
    mp_free_block_node_t *fn = free_node_arr(pool);

    uint16_t start = fn[idx].start_ppn;

    /* Empty list → head */
    if (mh->free_list_head == 0xFFFF) {
        mh->free_list_head = idx;
        fn[idx].next = 0xFFFF;
        return;
    }

    /* Insert before head */
    if (start < fn[mh->free_list_head].start_ppn) {
        fn[idx].next = mh->free_list_head;
        mh->free_list_head = idx;
        return;
    }

    /* Walk to find position */
    uint16_t prev = mh->free_list_head;
    uint16_t cur  = fn[prev].next;
    while (cur != 0xFFFF && fn[cur].start_ppn < start) {
        prev = cur;
        cur  = fn[cur].next;
    }

    fn[prev].next = idx;
    fn[idx].next  = cur;
}

/** Remove node idx from the free list.  prev_idx = 0xFFFF means idx is head. */
static void fb_list_remove(const mp_pool_t *pool, uint16_t prev_idx, uint16_t idx) {
    mp_meta_header_t   *mh = hdr(pool);
    mp_free_block_node_t *fn = free_node_arr(pool);

    if (prev_idx == 0xFFFF) {
        mh->free_list_head = fn[idx].next;
    } else {
        fn[prev_idx].next = fn[idx].next;
    }
    fn[idx].next = 0xFFFF;
}

/**
 * Try to merge node new_idx with adjacent free blocks.
 * Returns the merged node index (may be new_idx or a neighbour).
 */
static uint16_t fb_try_merge(const mp_pool_t *pool, uint16_t new_idx) {
    mp_meta_header_t   *mh = hdr(pool);
    mp_free_block_node_t *fn = free_node_arr(pool);

    uint16_t new_start = fn[new_idx].start_ppn;
    uint16_t new_end   = new_start + fn[new_idx].size - 1;

    /* Scan list for neighbours */
    uint16_t cur      = mh->free_list_head;
    uint16_t merge_prev = 0xFFFF;
    uint16_t merge_next = 0xFFFF;

    while (cur != 0xFFFF) {
        uint16_t cur_end = fn[cur].start_ppn + fn[cur].size - 1;

        /* cur is immediately before new_idx */
        if (cur_end + 1 == new_start)
            merge_prev = cur;
        /* cur is immediately after new_idx */
        if (new_end + 1 == fn[cur].start_ppn)
            merge_next = cur;

        cur  = fn[cur].next;
    }

    /* Case 1: merge with both neighbours */
    if (merge_prev != 0xFFFF && merge_next != 0xFFFF) {
        fn[merge_prev].size += fn[new_idx].size + fn[merge_next].size;
        /* Remove merge_next from list */
        uint16_t p = fb_find_prev(pool, mh->free_list_head, merge_next);
        fb_list_remove(pool, p, merge_next);
        fb_free_node(pool, merge_next);
        /* Remove new_idx (was inserted separately) */
        uint16_t p2 = fb_find_prev(pool, mh->free_list_head, new_idx);
        fb_list_remove(pool, p2, new_idx);
        fb_free_node(pool, new_idx);
        return merge_prev;
    }

    /* Case 2: merge with prev only */
    if (merge_prev != 0xFFFF) {
        fn[merge_prev].size += fn[new_idx].size;
        uint16_t p = fb_find_prev(pool, mh->free_list_head, new_idx);
        fb_list_remove(pool, p, new_idx);
        fb_free_node(pool, new_idx);
        return merge_prev;
    }

    /* Case 3: merge with next only */
    if (merge_next != 0xFFFF) {
        fn[new_idx].size += fn[merge_next].size;
        uint16_t p = fb_find_prev(pool, mh->free_list_head, merge_next);
        fb_list_remove(pool, p, merge_next);
        fb_free_node(pool, merge_next);
        return new_idx;
    }

    /* Case 4: no merge */
    return new_idx;
}

/* ── LRU list helpers (VM only) ──────────────────────────────── */

static void lru_remove(const mp_pool_t *pool, uint16_t hidx) {
    mp_meta_header_t  *mh = hdr(pool);
    mp_handle_entry_t *ht = handle_tbl(pool);

    if (ht[hidx].prev != 0xFFFF)
        ht[ht[hidx].prev].next = ht[hidx].next;
    else
        mh->lru_head = ht[hidx].next;

    if (ht[hidx].next != 0xFFFF)
        ht[ht[hidx].next].prev = ht[hidx].prev;
    else
        mh->lru_tail = ht[hidx].prev;

    ht[hidx].prev = 0xFFFF;
    ht[hidx].next = 0xFFFF;
}

static void lru_add_to_head(const mp_pool_t *pool, uint16_t hidx) {
    mp_meta_header_t  *mh = hdr(pool);
    mp_handle_entry_t *ht = handle_tbl(pool);

    ht[hidx].prev = 0xFFFF;
    ht[hidx].next = mh->lru_head;

    if (mh->lru_head != 0xFFFF)
        ht[mh->lru_head].prev = hidx;
    else
        mh->lru_tail = hidx;

    mh->lru_head = hidx;
}

static void lru_move_to_head(const mp_pool_t *pool, uint16_t hidx) {
    mp_meta_header_t *mh = hdr(pool);
    if (mh->lru_head == hidx) return; /* already head */
    lru_remove(pool, hidx);
    lru_add_to_head(pool, hidx);
}

static uint16_t lru_get_tail(const mp_pool_t *pool) {
    return hdr(pool)->lru_tail;
}

/* ── Handle validation ───────────────────────────────────────── */

static mp_handle_entry_t *validate_handle(const mp_pool_t *pool,
                                           mp_handle_t handle,
                                           mp_error_t *err) {
    mp_meta_header_t *mh = hdr(pool);

    if (handle.index >= mh->max_handles) {
        *err = MP_ERR_INVALID_HANDLE;
        return NULL;
    }

    mp_handle_entry_t *entry = &handle_tbl(pool)[handle.index];
    if (!entry->allocated || entry->generation != handle.generation) {
        *err = MP_ERR_INVALID_HANDLE;
        return NULL;
    }

    *err = MP_OK;
    return entry;
}

/* ── Misc helpers ────────────────────────────────────────────── */

static bool handle_all_pages_unlocked(const mp_pool_t *pool, uint16_t hidx) {
    mp_handle_entry_t *ht = handle_tbl(pool);
    uint16_t vpn   = ht[hidx].start_vpn;
    uint16_t count = ht[hidx].page_count;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t ppn = vpn2ppn_tbl(pool)[vpn + i];
        if (ppn != MP_PPN_INVALID && page_desc_tbl(pool)[ppn].lock_count > 0)
            return false;
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════ */

/* ── OOM notification helper ─────────────────────────────────── */
static inline void mp_oom_notify(mp_pool_t *pool, mp_error_t err,
                                  const char *file, int line) {
    if (!pool) return;
    mp_oom_callbacks_t *cb = &pool->oom_callbacks;
    if (cb->on_oom.fn) cb->on_oom.fn(cb->on_oom.user_data, err, file, line);
    switch (err) {
        case MP_ERR_NO_MEMORY:
            if (cb->on_no_memory.fn) cb->on_no_memory.fn(cb->on_no_memory.user_data, file, line);
            break;
        case MP_ERR_OUT_OF_HANDLES:
            if (cb->on_out_of_handles.fn) cb->on_out_of_handles.fn(cb->on_out_of_handles.user_data, file, line);
            break;
        case MP_ERR_FREE_NODE_EXHAUSTED:
            if (cb->on_node_exhausted.fn) cb->on_node_exhausted.fn(cb->on_node_exhausted.user_data, file, line);
            break;
        default: break;
    }
}

/* ── OOM callback setter ─────────────────────────────────────── */
mp_error_t mp_set_oom_callbacks_fn(mp_applicant_t *app,
                                    const mp_oom_callbacks_t *cb,
                                    const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    if (cb) {
        pool->oom_callbacks = *cb;
    } else {
        memset(&pool->oom_callbacks, 0, sizeof(pool->oom_callbacks));
    }
    return MP_OK;
}

/* ── mp_init ─────────────────────────────────────────────────── */
mp_error_t mp_init_fn(mp_pool_t *pool, const mp_config_t *cfg, const char *file, int line) {
    (void)file; (void)line;
    if (!pool || !cfg) return MP_ERR_NULL_PTR;
    if (pool->metadata) {
        /* Check if already initialised */
        mp_meta_header_t *m = (mp_meta_header_t *)pool->metadata;
        if (m->magic == MP_MAGIC) return MP_ERR_ALREADY_INIT;
    }

    /* Validate parameters */
    if (!cfg->pool_memory || !cfg->metadata) return MP_ERR_NULL_PTR;
    if (cfg->pool_size == 0 || cfg->page_size < 16) return MP_ERR_INVALID_PARAM;
    if (cfg->max_handles == 0) return MP_ERR_INVALID_PARAM;

    /* page_size must be a power of 2 */
    size_t ps = cfg->page_size;
    if ((ps & (ps - 1)) != 0) return MP_ERR_INVALID_PARAM;

    uint16_t total_pages = (uint16_t)(cfg->pool_size / cfg->page_size);
    if (total_pages == 0) return MP_ERR_INVALID_PARAM;

    /* Check metadata size */
    size_t needed = MP_METADATA_SIZE(total_pages, cfg->max_handles);
    if (cfg->metadata_size < needed) return MP_ERR_METADATA_TOO_SMALL;

    /* ── Populate pool structure ── */
    pool->pool_memory  = cfg->pool_memory;
    pool->pool_size    = cfg->pool_size;
    pool->metadata     = cfg->metadata;
    pool->metadata_size = cfg->metadata_size;
    pool->page_size    = cfg->page_size;
    pool->total_pages  = total_pages;
    pool->vm_enabled   = cfg->vm_enabled;
    pool->alloc_delayed = cfg->alloc_delayed ? 1 : 0;
    pool->delayed_no_reserve = cfg->delayed_no_reserve ? 1 : 0;
    pool->oom_callbacks      = cfg->oom_callbacks;
    pool->vm_user_data       = cfg->vm_user_data;
    pool->vm_evict     = cfg->vm_evict;
    pool->vm_load      = cfg->vm_load;

    /* ── Clear metadata ── */
    memset(cfg->metadata, 0, cfg->metadata_size);

    /* ── Init metadata header ── */
    mp_meta_header_t *mh = (mp_meta_header_t *)cfg->metadata;
    mh->magic        = MP_MAGIC;
    mh->version      = 1;
    mh->total_pages  = total_pages;
    mh->max_handles  = cfg->max_handles;
    mh->page_size    = (uint16_t)cfg->page_size;
    mh->next_applicant_id = 128;
    mh->reserved_pages    = 0;
    mh->free_list_head = 0xFFFF;
    mh->lru_head     = 0xFFFF;
    mh->lru_tail     = 0xFFFF;

    /* Compute table offsets */
    uint16_t off = (uint16_t)sizeof(mp_meta_header_t);
    mh->off_page_desc    = off; off = (uint16_t)(off + total_pages * sizeof(mp_page_desc_t));
    mh->off_handle_entry = off; off = (uint16_t)(off + cfg->max_handles * sizeof(mp_handle_entry_t));
    mh->off_vpn2ppn      = off; off = (uint16_t)(off + total_pages * sizeof(uint16_t));
    mh->off_ppn2vpn      = off; off = (uint16_t)(off + total_pages * sizeof(uint16_t));
    mh->off_free_nodes   = off; off = (uint16_t)(off + (cfg->max_handles + 1) * sizeof(mp_free_block_node_t));
    mh->off_handle_bm    = off; off = (uint16_t)(off + ((cfg->max_handles + 7) / 8));
    mh->off_node_bm      = off;

    /* ── Init Page Descriptor table ── */
    mp_page_desc_t *pd = page_desc_tbl(pool);
    for (uint16_t i = 0; i < total_pages; i++) {
        pd[i].flags         = 0;  /* MP_PAGE_FREE */
        pd[i].lock_count    = 0;
        pd[i].owner_handle  = 0xFFFF;
    }

    /* ── Init Handle Entry table ── */
    mp_handle_entry_t *ht = handle_tbl(pool);
    for (uint16_t i = 0; i < cfg->max_handles; i++) {
        ht[i].allocated = 0;
        ht[i].generation = 1;
    }

    /* ── Init VPN→PPN / PPN→VPN ── */
    uint16_t *v2p = vpn2ppn_tbl(pool);
    uint16_t *p2v = ppn2vpn_tbl(pool);
    for (uint16_t i = 0; i < total_pages; i++) {
        v2p[i] = MP_PPN_INVALID;
        p2v[i] = 0xFFFF;
    }

    /* ── Init free-block list ── */
    /* Node 0 = whole pool as one free block */
    mp_free_block_node_t *fn = free_node_arr(pool);
    fn[0].start_ppn = 0;
    fn[0].size      = total_pages;
    fn[0].next      = 0xFFFF;
    mh->free_list_head = 0;

    /* Node bitmap: all nodes free except node 0 */
    uint8_t *nbm = node_bm(pool);
    size_t nbm_bytes = (((size_t)cfg->max_handles + 1) + 7) / 8;
    memset(nbm, 0xFF, nbm_bytes);
    /* Mark node 0 as allocated */
    nbm[0] &= (uint8_t)~(1u << 0);

    /* Handle bitmap: all handles free */
    uint8_t *hbm = handle_bm(pool);
    size_t hbm_bytes = (((size_t)cfg->max_handles) + 7) / 8;
    memset(hbm, 0xFF, hbm_bytes);

    return MP_OK;
}

/* ── mp_applicant_create ─────────────────────────────────────── */
mp_error_t mp_applicant_create(mp_pool_t *pool, mp_applicant_t *app) {
    if (!pool || !app) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = (mp_meta_header_t *)pool->metadata;
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    app->pool = pool;
    app->applicant_id = mh->next_applicant_id;
    if (mh->next_applicant_id < 0xFFFF)
        mh->next_applicant_id++;
    return MP_OK;
}

/* ── mp_applicant_create_system ──────────────────────────────── */
mp_error_t mp_applicant_create_system(mp_pool_t *pool, mp_applicant_t *app,
                                       uint16_t system_id) {
    if (!pool || !app) return MP_ERR_NULL_PTR;
    if (system_id > 127) return MP_ERR_APPLICANT_ID_INVALID;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = (mp_meta_header_t *)pool->metadata;
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    app->pool = pool;
    app->applicant_id = system_id;
    return MP_OK;
}

/* ── Internal: evict victims until we have a contiguous block of `need` pages ── */
static mp_error_t vm_evict_until(mp_pool_t *pool, uint16_t need,
                                  uint16_t *out_ppn, uint16_t *out_size) {
    mp_meta_header_t *mh = hdr(pool);

    while (true) {
        /* Check free list for a block ≥ need */
        mp_free_block_node_t *fn = free_node_arr(pool);
        uint16_t cur = mh->free_list_head;
        while (cur != 0xFFFF) {
            if (fn[cur].size >= need) {
                *out_ppn  = fn[cur].start_ppn;
                *out_size = fn[cur].size;
                return MP_OK;
            }
            cur = fn[cur].next;
        }

        /* No suitable free block – evict one victim */
        uint16_t tail = lru_get_tail(pool);
        if (tail == 0xFFFF) return MP_ERR_NO_MEMORY;

        /* Scan from tail toward head for first fully unlocked handle */
        uint16_t victim = tail;
        while (victim != 0xFFFF && !handle_all_pages_unlocked(pool, victim)) {
            victim = handle_tbl(pool)[victim].prev;
        }
        if (victim == 0xFFFF) return MP_ERR_NO_VICTIM;

        mp_handle_entry_t *ve = &handle_tbl(pool)[victim];
        uint16_t vpn   = ve->start_vpn;
        uint16_t count = ve->page_count;
        uint16_t first_ppn = vpn2ppn_tbl(pool)[vpn];

        /* Evict */
        if (pool->vm_evict) {
            pool->vm_evict(pool->vm_user_data, vpn, count,
                           (uint8_t *)pool->pool_memory + first_ppn * pool->page_size,
                           (size_t)count * pool->page_size);
        }

        /* Mark pages as swapped out */
        for (uint16_t i = 0; i < count; i++) {
            uint16_t ppn = vpn2ppn_tbl(pool)[vpn + i];
            mp_page_desc_t *pd = &page_desc_tbl(pool)[ppn];
            pd->flags        = 2; /* MP_PAGE_SWAPPED */
            pd->owner_handle = victim;
            /* Update VPN→PPN: mark as invalid */
            vpn2ppn_tbl(pool)[vpn + i] = MP_PPN_INVALID;
            /* PPN→VPN: leave as is for now, will rebuild later */
            ppn2vpn_tbl(pool)[ppn] = 0xFFFF; /* mark PPN free */
        }

        /* Rebuild free block from evicted PPN range */
        uint16_t new_node = fb_alloc_node(pool);
        if (new_node == 0xFFFF) return MP_ERR_FREE_NODE_EXHAUSTED;
        fn[new_node].start_ppn = first_ppn;
        fn[new_node].size      = count;
        fn[new_node].next      = 0xFFFF;
        fb_list_insert(pool, new_node);
        fb_try_merge(pool, new_node);

        /* Remove victim from LRU list */
        lru_remove(pool, victim);
    }

    return MP_ERR_NO_MEMORY;
}

/* ── mp_alloc_pages ──────────────────────────────────────────── */
mp_error_t mp_alloc_pages_fn(mp_applicant_t *app, uint16_t num_pages, mp_handle_t *out, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool || !out) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;

    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;
    if (num_pages == 0) return MP_ERR_INVALID_PARAM;

    /* ── Delayed allocation: reserve handle but don't assign pages ── */
    if (pool->alloc_delayed) {
        mp_handle_entry_t *ht = handle_tbl(pool);
        uint16_t hidx = bm_alloc(handle_bm(pool), mh->max_handles);
        if (hidx == 0xFFFF) {

                {


                        mp_oom_notify(pool, MP_ERR_OUT_OF_HANDLES, file, line);


                        return MP_ERR_OUT_OF_HANDLES;


                    }

            }

        uint16_t gen = ht[hidx].generation;
        ht[hidx].generation    = gen;
        ht[hidx].start_vpn     = 0;
        ht[hidx].page_count    = num_pages;
        ht[hidx].last_access   = 0;
        ht[hidx].allocated     = 1;
        ht[hidx].applicant_id  = app->applicant_id;
        ht[hidx].delayed       = 1;
        ht[hidx].prev          = 0xFFFF;
        ht[hidx].next          = 0xFFFF;

        if (!pool->delayed_no_reserve)
            mh->reserved_pages += num_pages;

        out->index      = hidx;
        out->generation = gen;
        return MP_OK;
    }

    mp_free_block_node_t *fn = free_node_arr(pool);

    /* ── Try to allocate from free list ── */
    uint16_t found_ppn  = 0xFFFF;
    uint16_t found_size = 0;
    uint16_t found_node = 0xFFFF;
    uint16_t found_prev = 0xFFFF;

    {
        uint16_t prev = 0xFFFF;
        uint16_t cur  = mh->free_list_head;
        while (cur != 0xFFFF) {
            if (fn[cur].size >= num_pages) {
                found_ppn  = fn[cur].start_ppn;
                found_size = fn[cur].size;
                found_node = cur;
                found_prev = prev;
                break;
            }
            prev = cur;
            cur  = fn[cur].next;
        }
    }

    /* ── If no block found and VM enabled, try evicting ── */
    if (found_node == 0xFFFF && pool->vm_enabled) {
        mp_error_t er = vm_evict_until(pool, num_pages, &found_ppn, &found_size);
        if (er != MP_OK) {
            mp_oom_notify(pool, er, file, line);
            return er;
        }

        /* Re-scan for the newly created free block */
        uint16_t prev = 0xFFFF;
        uint16_t cur  = mh->free_list_head;
        found_node = 0xFFFF;
        while (cur != 0xFFFF) {
            if (fn[cur].start_ppn == found_ppn && fn[cur].size == found_size) {
                found_node = cur;
                found_prev = prev;
                break;
            }
            prev = cur;
            cur  = fn[cur].next;
        }
        if (found_node == 0xFFFF) {

                {


                        mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);


                        return MP_ERR_NO_MEMORY;


                    }

            }
    }

    if (found_node == 0xFFFF) {


            {



                    mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);



                    return MP_ERR_NO_MEMORY;



                }


        }

    /* ── Allocate from this block ── */
    uint16_t alloc_ppn = found_ppn;
    uint16_t remaining = found_size - num_pages;

    if (remaining > 0) {
        /* Split: shrink the existing node */
        fn[found_node].start_ppn = (uint16_t)(alloc_ppn + num_pages);
        fn[found_node].size      = remaining;
    } else {
        /* Exact fit: remove node */
        fb_list_remove(pool, found_prev, found_node);
        fb_free_node(pool, found_node);
    }

    /* ── Allocate handle entry ── */
    uint16_t hidx = bm_alloc(handle_bm(pool), mh->max_handles);
    if (hidx == 0xFFFF) {
        /* Roll back: return block to free list */
        uint16_t rn = fb_alloc_node(pool);
        if (rn != 0xFFFF) {
            fn[rn].start_ppn = alloc_ppn;
            fn[rn].size      = num_pages;
            fn[rn].next      = 0xFFFF;
            fb_list_insert(pool, rn);
        }
        {

                mp_oom_notify(pool, MP_ERR_OUT_OF_HANDLES, file, line);

                return MP_ERR_OUT_OF_HANDLES;

            }
    }

    mp_handle_entry_t *ht = handle_tbl(pool);
    uint16_t gen = ht[hidx].generation;

    /* Fill handle entry */
    ht[hidx].generation = gen;
    ht[hidx].start_vpn  = hidx; /* In no-VM mode VPN == handle index for simplicity;
                                    in VM mode we use proper VPN allocation */

    /* For simplicity in no-VM mode, VPN = first PPN of allocation.
       In VM mode we map the VPN range starting at a logical offset. */
    uint16_t start_vpn;
    if (pool->vm_enabled) {
        /* Use handle-index as VPN base for now; in a real VM each handle gets unique VPNs */
        start_vpn = alloc_ppn; /* one-to-one in basic mode */
    } else {
        start_vpn = alloc_ppn;
    }
    ht[hidx].start_vpn   = start_vpn;
    ht[hidx].page_count  = num_pages;
    ht[hidx].last_access = 0;
    ht[hidx].allocated   = 1;
    ht[hidx].applicant_id = app->applicant_id;
    ht[hidx].delayed      = 0;
    ht[hidx].prev        = 0xFFFF;
    ht[hidx].next        = 0xFFFF;
    ht[hidx].full_locked = 0;
    ht[hidx].parent_handle = 0xFFFF;
    ht[hidx].child_type    = 0;
    ht[hidx].child_refs    = 0;
    ht[hidx].child_wr_refs = 0;

    /* Add to LRU head if VM enabled */
    if (pool->vm_enabled)
        lru_add_to_head(pool, hidx);

    /* ── Update page descriptors and mapping tables ── */
    mp_page_desc_t *pd = page_desc_tbl(pool);
    uint16_t *v2p = vpn2ppn_tbl(pool);
    uint16_t *p2v = ppn2vpn_tbl(pool);

    for (uint16_t i = 0; i < num_pages; i++) {
        uint16_t ppn = alloc_ppn + i;
        uint16_t vpn = start_vpn + i;

        pd[ppn].flags         = 1; /* MP_PAGE_ALLOCATED */
        pd[ppn].lock_count    = 0;
        pd[ppn].owner_handle  = hidx;

        v2p[vpn] = ppn;
        p2v[ppn] = vpn;
    }

    /* ── Return handle ── */
    out->index      = hidx;
    out->generation = gen;

    return MP_OK;
}

/* ── mp_alloc ────────────────────────────────────────────────── */
mp_error_t mp_alloc_fn(mp_applicant_t *app, size_t size, mp_handle_t *out, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool || !out) return MP_ERR_NULL_PTR;
    if (size == 0) return MP_ERR_INVALID_PARAM;

    uint16_t num_pages = (uint16_t)((size + pool->page_size - 1) / pool->page_size);
    if (num_pages == 0) num_pages = 1;

    return mp_alloc_pages_fn(app, num_pages, out, file, line);
}

/* ── mp_lock_fn ────────────────────────────────────────────────── */
mp_error_t mp_lock_fn(mp_applicant_t *app, mp_handle_t handle, void **out_ptr, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_error_t err;
    mp_handle_entry_t *entry = validate_handle(pool, handle, &err);
    if (!entry) return err;

    /* Mutex: root handle with children → no full lock */
    if (entry->parent_handle == 0xFFFF && entry->child_refs > 0)
        return MP_ERR_WR_LOCKED;
    /* Child handle: parent must not be full_locked (safety check) */
    if (entry->parent_handle != 0xFFFF) {
        mp_handle_entry_t *parents = handle_tbl(pool);
        if (parents[entry->parent_handle].full_locked > 0)
            return MP_ERR_WR_LOCKED;
    }
    /* Max concurrent locks check */
    if (entry->full_locked >= MP_FULL_LOCK_MAX)
        return MP_ERR_WR_LOCKED;

    mp_page_desc_t *pd  = page_desc_tbl(pool);
    uint16_t       *v2p = vpn2ppn_tbl(pool);
    uint16_t       *p2v = ppn2vpn_tbl(pool);

    /* ── Handle delayed allocation (first lock) ──────────────── */
    if (entry->delayed) {
        uint16_t count = entry->page_count;
        mp_free_block_node_t *fn = free_node_arr(pool);

        /* Find a free block big enough */
        uint16_t prev = 0xFFFF;
        uint16_t cur  = mh->free_list_head;
        uint16_t found_ppn   = 0xFFFF;
        uint16_t found_node  = 0xFFFF;
        uint16_t found_prev  = 0xFFFF;

        while (cur != 0xFFFF) {
            if (fn[cur].size >= count) {
                found_ppn  = fn[cur].start_ppn;
                found_node = cur;
                found_prev = prev;
                break;
            }
            prev = cur;
            cur  = fn[cur].next;
        }

        if (found_node == 0xFFFF && pool->vm_enabled) {
            uint16_t out_ppn, out_size;
            mp_error_t er = vm_evict_until(pool, count, &out_ppn, &out_size);
            if (er != MP_OK) {
                mp_oom_notify(pool, er, file, line);
                return er;
            }
            prev = 0xFFFF; cur = mh->free_list_head;
            found_node = 0xFFFF;
            while (cur != 0xFFFF) {
                if (fn[cur].start_ppn == out_ppn && fn[cur].size == out_size) {
                    found_node = cur; found_prev = prev; found_ppn = out_ppn;
                    break;
                }
                prev = cur; cur = fn[cur].next;
            }
        }
        if (found_node == 0xFFFF) {

                {


                        mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);


                        return MP_ERR_NO_MEMORY;


                    }

            }

        /* Split free block */
        uint16_t remaining = fn[found_node].size - count;
        if (remaining > 0) {
            fn[found_node].start_ppn = (uint16_t)(found_ppn + count);
            fn[found_node].size      = remaining;
        } else {
            fb_list_remove(pool, found_prev, found_node);
            fb_free_node(pool, found_node);
        }

        /* Set up page descriptors and mapping tables */
        uint16_t start_vpn = found_ppn;  /* no-VM: VPN == PPN */
        if (pool->vm_enabled)
            start_vpn = entry->start_vpn;  /* VM: preserve logical VPN */

        for (uint16_t i = 0; i < count; i++) {
            uint16_t ppn = found_ppn + i;
            uint16_t vpn = start_vpn  + i;
            pd[ppn].flags        = 1;
            pd[ppn].lock_count   = 0;  /* incremented below */
            pd[ppn].owner_handle = handle.index;
            v2p[vpn] = ppn;
            p2v[ppn] = vpn;
        }

        entry->start_vpn = start_vpn;
        entry->delayed   = 0;

        if (!pool->delayed_no_reserve)
            mh->reserved_pages -= count;

        /* Lock all pages (count is entry->page_count) */
        for (uint16_t i = 0; i < count; i++) {
            uint16_t ppn = v2p[start_vpn + i];
            if (ppn != MP_PPN_INVALID) pd[ppn].lock_count++;
        }

        if (pool->vm_enabled)
            lru_move_to_head(pool, handle.index);

        if (out_ptr)
            *out_ptr = (uint8_t *)pool->pool_memory + (size_t)found_ppn * pool->page_size;
        entry->full_locked++;
        return MP_OK;
    }

    uint16_t vpn   = entry->start_vpn;
    uint16_t count = entry->page_count;
    uint16_t first_ppn = 0xFFFF;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t ppn = v2p[vpn + i];

        /* VM: load if swapped out */
        if (ppn == MP_PPN_INVALID && pool->vm_enabled) {
            /* Need to find a free PPN to load into */
            /* Scan free list for any free page(s) */
            mp_free_block_node_t *fn = free_node_arr(pool);
            uint16_t fcur = mh->free_list_head;
            bool loaded = false;

            while (fcur != 0xFFFF) {
                if (fn[fcur].size > 0) {
                    /* Use first page of this free block */
                    uint16_t load_ppn = fn[fcur].start_ppn;

                    /* Shrink the free block */
                    fn[fcur].start_ppn++;
                    fn[fcur].size--;

                    if (fn[fcur].size == 0) {
                        uint16_t p = fb_find_prev(pool, mh->free_list_head, fcur);
                        fb_list_remove(pool, p, fcur);
                        fb_free_node(pool, fcur);
                    }

                    /* Load one page */
                    if (pool->vm_load) {
                        pool->vm_load(pool->vm_user_data, (uint16_t)(vpn + i),
                                      1, (uint8_t *)pool->pool_memory + load_ppn * pool->page_size,
                                      pool->page_size);
                    }

                    v2p[vpn + i] = load_ppn;
                    ppn2vpn_tbl(pool)[load_ppn] = (uint16_t)(vpn + i);
                    pd[load_ppn].flags        = 1;
                    pd[load_ppn].lock_count   = 0;
                    pd[load_ppn].owner_handle = handle.index;

                    ppn = load_ppn;
                    loaded = true;
                    break;
                }
                fcur = fn[fcur].next;
            }

            if (!loaded) {


                    {



                            mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);



                            return MP_ERR_NO_MEMORY;



                        }


                }
        } else if (ppn == MP_PPN_INVALID) {
            return MP_ERR_INVALID_POOL; /* page missing but VM not enabled */
        }

        pd[ppn].lock_count++;
        if (i == 0) first_ppn = ppn;
    }

    /* Move to LRU head */
    if (pool->vm_enabled) {
        lru_move_to_head(pool, handle.index);
    }

    if (out_ptr) {
        *out_ptr = (uint8_t *)pool->pool_memory + (size_t)first_ppn * pool->page_size;
    }

    entry->full_locked++;
    return MP_OK;
}

/* ── mp_partial_map_fn (child handle from parent sub-range) ──── */
mp_error_t mp_partial_map_fn(mp_applicant_t *app, mp_handle_t parent,
                            size_t offset, size_t length, bool writable,
                            mp_handle_t *out_child, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;
    if (!out_child) return MP_ERR_NULL_PTR;
    if (length == 0) return MP_ERR_INVALID_PARAM;

    mp_error_t err;
    mp_handle_entry_t *parent_entry = validate_handle(pool, parent, &err);
    if (!parent_entry) return err;

    /* Must be a root handle */
    if (parent_entry->parent_handle != 0xFFFF)
        return MP_ERR_INVALID_PARAM;

    size_t total_bytes = (size_t)parent_entry->page_count * pool->page_size;
    if (offset >= total_bytes) return MP_ERR_OUT_OF_RANGE;
    if (offset + length > total_bytes)
        length = total_bytes - offset;

    /* Mutex: parent full_locked → no children */
    if (parent_entry->full_locked)
        return MP_ERR_WR_LOCKED;
    /* Writable child → parent must have no children at all */
    if (writable && parent_entry->child_refs > 0)
        return MP_ERR_WR_LOCKED;
    /* Read-only child → parent must have no writable children */
    if (!writable && parent_entry->child_wr_refs > 0)
        return MP_ERR_WR_LOCKED;

    /* Allocate a handle entry for the child */
    uint16_t child_idx = bm_alloc(handle_bm(pool), mh->max_handles);
    if (child_idx == 0xFFFF)
        return MP_ERR_OUT_OF_HANDLES;

    mp_handle_entry_t *child = &handle_tbl(pool)[child_idx];

    uint16_t start_vpn_off = (uint16_t)(offset / pool->page_size);
    uint16_t num_pages     = (uint16_t)((length + pool->page_size - 1) / pool->page_size);

    child->generation     = parent_entry->generation;
    child->start_vpn      = (uint16_t)(parent_entry->start_vpn + start_vpn_off);
    child->page_count     = num_pages;
    child->last_access    = 0;
    child->prev           = 0xFFFF;
    child->next           = 0xFFFF;
    child->applicant_id   = app->applicant_id;
    child->allocated      = 1;
    child->delayed        = 0;
    child->full_locked    = 0;
    child->parent_handle  = parent.index;
    child->child_type     = writable ? 1 : 0;
    child->child_refs     = 0;
    child->child_wr_refs  = 0;

    /* Update parent bookkeeping */
    parent_entry->child_refs++;
    if (writable)
        parent_entry->child_wr_refs++;

    out_child->index      = child_idx;
    out_child->generation = child->generation;
    return MP_OK;
}

/* ── mp_unlock ─────────────────────────────────────────────────── */
mp_error_t mp_unlock_fn(mp_applicant_t *app, mp_handle_t handle, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_error_t err;
    mp_handle_entry_t *entry = validate_handle(pool, handle, &err);
    if (!entry) return err;

    uint16_t vpn   = entry->start_vpn;
    uint16_t count = entry->page_count;
    uint16_t *v2p  = vpn2ppn_tbl(pool);
    mp_page_desc_t *pd = page_desc_tbl(pool);

    bool any_unlocked = false;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t ppn = v2p[vpn + i];
        if (ppn == MP_PPN_INVALID) continue;
        if (pd[ppn].lock_count == 0) continue;
        pd[ppn].lock_count--;
        any_unlocked = true;
    }

    if (entry->full_locked > 0)
        entry->full_locked--;

    /* Writable child: vm_evict after unlock */
    if (entry->parent_handle != 0xFFFF && entry->child_type == 1
        && any_unlocked && pool->vm_enabled && pool->vm_evict) {
        uint8_t *base = (uint8_t *)pool->pool_memory;
        uint16_t first_ppn = v2p[entry->start_vpn];
        if (first_ppn != MP_PPN_INVALID) {
            pool->vm_evict(pool->vm_user_data,
                           entry->start_vpn, entry->page_count,
                           base + (size_t)first_ppn * pool->page_size,
                           (size_t)entry->page_count * pool->page_size);
        }
    }

    return any_unlocked ? MP_OK : MP_ERR_NOT_LOCKED;
}

/* ── mp_free ─────────────────────────────────────────────────── */
mp_error_t mp_free_fn(mp_applicant_t *app, mp_handle_t handle, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_error_t err;
    mp_handle_entry_t *entry = validate_handle(pool, handle, &err);
    if (!entry) return err;

    /* ── Child handle: unmap only, don't free parent's pages ── */
    if (entry->parent_handle != 0xFFFF) {
        uint16_t vpn   = entry->start_vpn;
        uint16_t count = entry->page_count;
        uint16_t *v2p  = vpn2ppn_tbl(pool);
        mp_page_desc_t *pd = page_desc_tbl(pool);
        for (uint16_t i = 0; i < count; i++) {
            uint16_t ppn = v2p[vpn + i];
            if (ppn != MP_PPN_INVALID && pd[ppn].lock_count > 0)
                pd[ppn].lock_count--;
        }
        mp_handle_entry_t *parents = handle_tbl(pool);
        parents[entry->parent_handle].child_refs--;
        if (entry->child_type == 1)
            parents[entry->parent_handle].child_wr_refs--;
        goto free_handle;
    }

    /* ── Delayed handle – no pages to free ── */
    if (entry->delayed) {
        if (!pool->delayed_no_reserve)
            mh->reserved_pages -= entry->page_count;
        goto free_handle;
    }

    uint16_t vpn   = entry->start_vpn;
    uint16_t count = entry->page_count;
    uint16_t *v2p  = vpn2ppn_tbl(pool);
    uint16_t *p2v  = ppn2vpn_tbl(pool);
    mp_page_desc_t *pd = page_desc_tbl(pool);
    mp_free_block_node_t *fn = free_node_arr(pool);

    /* Find the PPN range for this handle */
    uint16_t first_ppn = v2p[vpn];
    uint16_t contiguous = count;

    /* If VM mode and pages are swapped out, we still need to find PPNs */
    if (first_ppn == MP_PPN_INVALID && pool->vm_enabled) {
        /* Pages were swapped out – just mark handle free and return */
        goto free_handle;
    }

    /* Clear page descriptors and mappings */
    for (uint16_t i = 0; i < count; i++) {
        uint16_t ppn = v2p[vpn + i];
        if (ppn != MP_PPN_INVALID) {
            pd[ppn].flags        = 0;
            pd[ppn].lock_count   = 0;
            pd[ppn].owner_handle = 0xFFFF;
            p2v[ppn] = 0xFFFF;
            v2p[vpn + i] = MP_PPN_INVALID;
        }
    }

    /* Create a free-block node for the freed pages */
    {
        uint16_t nidx = fb_alloc_node(pool);
        if (nidx == 0xFFFF) {

                {


                        mp_oom_notify(pool, MP_ERR_FREE_NODE_EXHAUSTED, file, line);


                        return MP_ERR_FREE_NODE_EXHAUSTED;


                    }

            }
        fn[nidx].start_ppn = first_ppn;
        fn[nidx].size      = contiguous;
        fn[nidx].next      = 0xFFFF;
        fb_list_insert(pool, nidx);
        fb_try_merge(pool, nidx);
    }

free_handle:
    /* Remove from LRU list if VM enabled */
    if (pool->vm_enabled)
        lru_remove(pool, handle.index);

    /* Invalidate handle entry */
    entry->generation++;
    entry->allocated     = 0;
    entry->delayed       = 0;
    entry->full_locked   = 0;
    entry->parent_handle = 0xFFFF;
    entry->child_type    = 0;
    entry->child_refs    = 0;
    entry->child_wr_refs = 0;
    entry->start_vpn     = 0;
    entry->page_count    = 0;
    entry->prev          = 0xFFFF;
    entry->next          = 0xFFFF;

    /* Free handle slot in bitmap */
    bm_free(handle_bm(pool), handle.index);

    return MP_OK;
}

/* ── Internal: relocate handle to a different PPN range ────── */
static mp_error_t handle_relocate(mp_pool_t *pool, uint16_t hidx,
                                   uint16_t new_ppn, uint16_t new_count) {
    mp_handle_entry_t  *ht  = handle_tbl(pool);
    mp_page_desc_t     *pd  = page_desc_tbl(pool);
    mp_free_block_node_t *fn  = free_node_arr(pool);
    uint16_t           *v2p = vpn2ppn_tbl(pool);
    uint16_t           *p2v = ppn2vpn_tbl(pool);

    uint16_t old_vpn  = ht[hidx].start_vpn;
    uint16_t old_cnt  = ht[hidx].page_count;
    uint16_t old_ppn  = v2p[old_vpn];
    size_t   copy_sz  = ((size_t)(old_cnt < new_count ? old_cnt : new_count))
                         * pool->page_size;

    /* Move data */
    if (old_ppn != new_ppn && copy_sz > 0) {
        uint8_t *base = (uint8_t *)pool->pool_memory;
        memmove(base + (size_t)new_ppn * pool->page_size,
                base + (size_t)old_ppn * pool->page_size, copy_sz);
    }

    /* Release old pages */
    for (uint16_t i = 0; i < old_cnt; i++) {
        uint16_t ppn = v2p[old_vpn + i];
        if (ppn != MP_PPN_INVALID) {
            pd[ppn].flags        = 0;
            pd[ppn].lock_count   = 0;
            pd[ppn].owner_handle = 0xFFFF;
            p2v[ppn] = 0xFFFF;
        }
        v2p[old_vpn + i] = MP_PPN_INVALID;
    }

    /* Return old pages to free list */
    {
        uint16_t nidx = fb_alloc_node(pool);
        if (nidx != 0xFFFF) {
            fn[nidx].start_ppn = old_ppn;
            fn[nidx].size      = old_cnt;
            fn[nidx].next      = 0xFFFF;
            fb_list_insert(pool, nidx);
            fb_try_merge(pool, nidx);
        }
    }

    /* Claim new pages.
     *
     * In no-VM mode (VPN == PPN), the handle's start_vpn moves to new_ppn,
     * so VPN→PPN is vpn2ppn[new_ppn + i] = new_ppn + i.
     *
     * In VM mode, the handle's VPN persists as a logical key; the physical
     * location changes: vpn2ppn[old_vpn + i] = new_ppn + i. */
    if (!pool->vm_enabled) {
        for (uint16_t i = 0; i < new_count; i++) {
            uint16_t ppn  = new_ppn + i;
            uint16_t vpn  = new_ppn + i;   /* VPN == PPN */
            pd[ppn].flags        = 1;
            pd[ppn].lock_count   = 0;
            pd[ppn].owner_handle = hidx;
            v2p[vpn] = ppn;
            p2v[ppn] = vpn;
        }
        ht[hidx].start_vpn  = new_ppn;
        ht[hidx].page_count = new_count;
    } else {
        for (uint16_t i = 0; i < new_count; i++) {
            uint16_t ppn  = new_ppn + i;
            uint16_t vpn  = old_vpn + i;
            pd[ppn].flags        = 1;
            pd[ppn].lock_count   = 0;
            pd[ppn].owner_handle = hidx;
            v2p[vpn] = ppn;
            p2v[ppn] = vpn;
        }
        ht[hidx].page_count = new_count;
        /* start_vpn unchanged in VM mode */
    }

    return MP_OK;
}

/* ── Internal: shrink free block that overlaps [start, end) ── */
static void fb_shrink_overlap(mp_pool_t *pool, uint16_t start, uint16_t end) {
    mp_meta_header_t   *mh = hdr(pool);
    mp_free_block_node_t *fn = free_node_arr(pool);

    uint16_t prev = 0xFFFF;
    uint16_t cur  = mh->free_list_head;
    while (cur != 0xFFFF) {
        uint16_t fb_s = fn[cur].start_ppn;
        uint16_t fb_e = fb_s + fn[cur].size - 1;

        if (fb_e < start || fb_s >= end) {
            prev = cur; cur = fn[cur].next;
            continue;
        }

        /* Overlap region */
        if (fb_s >= start && fb_e < end) {
            /* Whole block consumed */
            uint16_t nxt = fn[cur].next;
            fb_list_remove(pool, prev, cur);
            fb_free_node(pool, cur);
            cur = nxt;
        } else if (fb_s < start && fb_e >= end) {
            /* Split: keep left part, remove overlap */
            uint16_t right_sz = fb_e - end + 1;
            fn[cur].size = start - fb_s;

            if (right_sz > 0) {
                uint16_t nidx = fb_alloc_node(pool);
                if (nidx != 0xFFFF) {
                    fn[nidx].start_ppn = end;
                    fn[nidx].size      = right_sz;
                    fn[nidx].next      = fn[cur].next;
                    fn[cur].next       = nidx;
                }
            }
            prev = cur; cur = fn[cur].next;
        } else if (fb_s >= start) {
            /* Remove head of free block */
            uint16_t consume = end - fb_s;
            fn[cur].start_ppn += consume;
            fn[cur].size      -= consume;
            prev = cur; cur = fn[cur].next;
        } else {
            /* Remove tail of free block */
            fn[cur].size = start - fb_s;
            prev = cur; cur = fn[cur].next;
        }
    }
}

/* ── mp_resize_pages ─────────────────────────────────────────── */
mp_error_t mp_resize_pages_fn(mp_applicant_t *app, mp_handle_t *handle,
                            uint16_t new_num_pages, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool || !handle) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;
    if (new_num_pages == 0) return MP_ERR_INVALID_PARAM;

    mp_error_t err;
    mp_handle_entry_t *entry = validate_handle(pool, *handle, &err);
    if (!entry) return err;

    uint16_t old_cnt = entry->page_count;
    if (new_num_pages == old_cnt) return MP_OK;

    mp_page_desc_t     *pd  = page_desc_tbl(pool);
    mp_free_block_node_t *fn  = free_node_arr(pool);
    uint16_t           *v2p = vpn2ppn_tbl(pool);
    uint16_t           *p2v = ppn2vpn_tbl(pool);
    uint16_t vpn      = entry->start_vpn;
    uint16_t first_ppn = v2p[vpn];

    /* ── SHRINK ───────────────────────────────────────────────── */
    if (new_num_pages < old_cnt) {
        uint16_t free_start = first_ppn + new_num_pages;
        uint16_t free_cnt   = old_cnt - new_num_pages;

        for (uint16_t i = 0; i < free_cnt; i++) {
            uint16_t ppn = free_start + i;
            if (p2v[ppn] != 0xFFFF) v2p[p2v[ppn]] = MP_PPN_INVALID;
            pd[ppn].flags        = 0;
            pd[ppn].lock_count   = 0;
            pd[ppn].owner_handle = 0xFFFF;
            p2v[ppn] = 0xFFFF;
        }

        uint16_t nidx = fb_alloc_node(pool);
        if (nidx == 0xFFFF) {

                {


                        mp_oom_notify(pool, MP_ERR_FREE_NODE_EXHAUSTED, file, line);


                        return MP_ERR_FREE_NODE_EXHAUSTED;


                    }

            }
        fn[nidx].start_ppn = free_start;
        fn[nidx].size      = free_cnt;
        fn[nidx].next      = 0xFFFF;
        fb_list_insert(pool, nidx);
        fb_try_merge(pool, nidx);

        entry->page_count = new_num_pages;
        return MP_OK;
    }

    /* ── ENLARGE ──────────────────────────────────────────────── */
    uint16_t extra   = new_num_pages - old_cnt;
    uint16_t end_ppn = first_ppn + old_cnt;

    /* Clip to pool boundary */
    if ((uint32_t)end_ppn + extra > mh->total_pages) {
        {

                mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);

                return MP_ERR_NO_MEMORY;

            }
    }

    /* Scan the expansion window [end_ppn, end_ppn + extra) */
    bool     all_free      = true;
    bool     has_locked    = false;
    uint16_t in_way_handles[32];
    uint16_t num_in_way    = 0;

    for (uint16_t i = 0; i < extra; i++) {
        uint16_t ppn = end_ppn + i;
        if (pd[ppn].flags != 0) {
            all_free = false;
            if (pd[ppn].lock_count > 0) { has_locked = true; break; }
            uint16_t owner = pd[ppn].owner_handle;
            uint16_t j;
            for (j = 0; j < num_in_way; j++)
                if (in_way_handles[j] == owner) break;
            if (j == num_in_way && num_in_way < 32)
                in_way_handles[num_in_way++] = owner;
        }
    }

    if (has_locked) {


            {



                    mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);



                    return MP_ERR_NO_MEMORY;



                }


        }

    /* ── Case A: all free → extend in place ── */
    if (all_free) {
        fb_shrink_overlap(pool, end_ppn, end_ppn + extra);

        for (uint16_t i = 0; i < extra; i++) {
            uint16_t ppn  = end_ppn + i;
            uint16_t newv = vpn + old_cnt + i;
            pd[ppn].flags        = 1;
            pd[ppn].lock_count   = 0;
            pd[ppn].owner_handle = handle->index;
            v2p[newv] = ppn;
            p2v[ppn]  = newv;
        }

        entry->page_count = new_num_pages;
        return MP_OK;
    }

    /* ── Case B: unlocked handles in the way ── */
    /* Calculate total size of handles in the way */
    uint16_t way_total = 0;
    for (uint16_t i = 0; i < num_in_way; i++)
        way_total += handle_tbl(pool)[in_way_handles[i]].page_count;

    if (old_cnt <= way_total) {
        /* Move current handle to a new location */
        /* Find new_count contiguous free pages */
        uint16_t prev = 0xFFFF;
        uint16_t cur  = mh->free_list_head;
        bool found = false;
        while (cur != 0xFFFF) {
            if (fn[cur].size >= new_num_pages) {
                found = true;
                break;
            }
            prev = cur;
            cur  = fn[cur].next;
        }
        if (!found) {

                {


                        mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);


                        return MP_ERR_NO_MEMORY;


                    }

            }

        uint16_t new_ppn = fn[cur].start_ppn;

        /* Split free block */
        if (fn[cur].size > new_num_pages) {
            fn[cur].start_ppn += new_num_pages;
            fn[cur].size      -= new_num_pages;
        } else {
            fb_list_remove(pool, prev, cur);
            fb_free_node(pool, cur);
        }

        /* Relocate */
        err = handle_relocate(pool, handle->index, new_ppn, new_num_pages);
        if (err != MP_OK) return err;

        /* Update LRU if VM enabled */
        if (pool->vm_enabled)
            lru_move_to_head(pool, handle->index);

        return MP_OK;
    }

    /* Move handles in the way out */
    for (uint16_t i = 0; i < num_in_way; i++) {
        uint16_t hidx     = in_way_handles[i];
        uint16_t h_count  = handle_tbl(pool)[hidx].page_count;

        /* Find free space */
        uint16_t prev = 0xFFFF;
        uint16_t cur  = mh->free_list_head;
        bool found = false;
        while (cur != 0xFFFF) {
            if (fn[cur].size >= h_count) {
                found = true;
                break;
            }
            prev = cur;
            cur  = fn[cur].next;
        }
        if (!found) {

                {


                        mp_oom_notify(pool, MP_ERR_NO_MEMORY, file, line);


                        return MP_ERR_NO_MEMORY;


                    }

            }

        uint16_t new_ppn = fn[cur].start_ppn;

        /* Split free block */
        if (fn[cur].size > h_count) {
            fn[cur].start_ppn += h_count;
            fn[cur].size      -= h_count;
        } else {
            fb_list_remove(pool, prev, cur);
            fb_free_node(pool, cur);
        }

        /* Relocate this handle */
        err = handle_relocate(pool, hidx, new_ppn, h_count);
        if (err != MP_OK) return err;

        if (pool->vm_enabled)
            lru_move_to_head(pool, hidx);
    }

    /* Now the expansion window should be free – extend in place */
    fb_shrink_overlap(pool, end_ppn, end_ppn + extra);
    for (uint16_t i = 0; i < extra; i++) {
        uint16_t ppn  = end_ppn + i;
        uint16_t newv = vpn + old_cnt + i;
        pd[ppn].flags        = 1;
        pd[ppn].lock_count   = 0;
        pd[ppn].owner_handle = handle->index;
        v2p[newv] = ppn;
        p2v[ppn]  = newv;
    }
    entry->page_count = new_num_pages;
    return MP_OK;
}

/* ── mp_resize ───────────────────────────────────────────────── */

    mp_error_t mp_resize_fn(mp_applicant_t *app, mp_handle_t *handle, size_t new_size, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool || !handle) return MP_ERR_NULL_PTR;
    if (new_size == 0) return MP_ERR_INVALID_PARAM;

    uint16_t np = (uint16_t)((new_size + pool->page_size - 1) / pool->page_size);
    if (np == 0) np = 1;
    return mp_resize_pages_fn(app, handle, np, file, line);
}

/* ── mp_compact_fn ──────────────────────────────────────────────── */
mp_error_t mp_compact_fn(mp_applicant_t *app, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_page_desc_t      *pd  = page_desc_tbl(pool);
    mp_free_block_node_t *fn  = free_node_arr(pool);
    uint16_t            *v2p = vpn2ppn_tbl(pool);
    uint16_t            *p2v = ppn2vpn_tbl(pool);
    uint8_t             *nbm = node_bm(pool);
    size_t nbm_bytes = (((size_t)mh->max_handles + 1) + 7) / 8;
    uint16_t tp = mh->total_pages;

    /* ── Clear existing free list, recycle all nodes ── */
    memset(nbm, 0xFF, nbm_bytes);
    mh->free_list_head = 0xFFFF;

    /* ── Compact: move unlocked allocated blocks toward PPN 0 ── */
    uint16_t write_ppn = 0;

    for (uint16_t read_ppn = 0; read_ppn < tp; ) {
        /* Free or swapped page – skip */
        if (pd[read_ppn].flags == 0 || pd[read_ppn].flags == 2) {
            read_ppn++;
            continue;
        }

        /* Locked page – skip block entirely, advance write cursor */
        if (pd[read_ppn].lock_count > 0) {
            uint16_t block_start = read_ppn;
            while (read_ppn < tp && pd[read_ppn].lock_count > 0)
                read_ppn++;
            if (write_ppn < block_start)
                write_ppn = block_start;
            write_ppn = read_ppn;
            continue;
        }

        /* Unlocked, allocated page – find contiguous block */
        uint16_t block_start = read_ppn;
        uint16_t handle_idx  = pd[read_ppn].owner_handle;
        uint16_t block_count = 0;

        while (read_ppn < tp && pd[read_ppn].flags == 1 &&
               pd[read_ppn].lock_count == 0 &&
               pd[read_ppn].owner_handle == handle_idx) {
            read_ppn++;
            block_count++;
        }

        /* Move block to write_ppn if needed */
        if (block_start != write_ppn) {
            size_t block_bytes = (size_t)block_count * pool->page_size;
            void *src = (uint8_t *)pool->pool_memory
                        + (size_t)block_start * pool->page_size;
            void *dst = (uint8_t *)pool->pool_memory
                        + (size_t)write_ppn * pool->page_size;
            memmove(dst, src, block_bytes);

            for (uint16_t i = 0; i < block_count; i++) {
                uint16_t old_ppn = block_start + i;
                uint16_t new_ppn = write_ppn + i;
                uint16_t vpn     = p2v[old_ppn];

                p2v[new_ppn] = vpn;
                p2v[old_ppn] = 0xFFFF;
                if (vpn != 0xFFFF)
                    v2p[vpn] = new_ppn;

                pd[new_ppn] = pd[old_ppn];
                pd[old_ppn].flags        = 0;
                pd[old_ppn].lock_count   = 0;
                pd[old_ppn].owner_handle = 0xFFFF;
            }
        }

        write_ppn += block_count;
    }

    /* ── Rebuild free block list from space after write_ppn ── */
    {
        uint16_t i = write_ppn;
        while (i < tp) {
            while (i < tp && pd[i].flags != 0) i++;
            if (i >= tp) break;

            uint16_t free_start = i;
            while (i < tp && pd[i].flags == 0) i++;
            uint16_t free_size = (uint16_t)(i - free_start);

            uint16_t nidx = bm_alloc(nbm, (uint16_t)(mh->max_handles + 1));
            if (nidx == 0xFFFF) {

                    {


                            mp_oom_notify(pool, MP_ERR_FREE_NODE_EXHAUSTED, file, line);


                            return MP_ERR_FREE_NODE_EXHAUSTED;


                        }

                }

            fn[nidx].start_ppn = free_start;
            fn[nidx].size      = free_size;
            fn[nidx].next      = 0xFFFF;

            /* Append to end of list (already in order) */
            if (mh->free_list_head == 0xFFFF) {
                mh->free_list_head = nidx;
            } else {
                uint16_t cur = mh->free_list_head;
                while (fn[cur].next != 0xFFFF) cur = fn[cur].next;
                fn[cur].next = nidx;
            }
        }
    }

    return MP_OK;
}

/* ── mp_get_si
    ze ─────────────────────────────────────────────── */
mp_error_t mp_get_size_fn(mp_applicant_t *app, mp_handle_t handle, size_t *out_size, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool || !out_size) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_error_t err;
    mp_handle_entry_t *entry = validate_handle(pool, handle, &err);
    if (!entry) return err;

    *out_size = (size_t)entry->page_count * pool->page_size;
    return MP_OK;
}

/* ── mp_get_page_count ───────────────────────────────────────── */
mp_error_t mp_get_page_count_fn(mp_applicant_t *app, mp_handle_t handle,
                              uint16_t *out_pages, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool || !out_pages) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_error_t err;
    mp_handle_entry_t *entry = validate_handle(pool, handle, &err);
    if (!entry) return err;

    *out_pages = entry->page_count;
    return MP_OK;
}

/* ── mp_get_ptr (only when locked, does not increment lock) ──── */
mp_error_t mp_get_ptr_fn(mp_applicant_t *app, mp_handle_t handle, void **out_ptr, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool || !out_ptr) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_error_t err;
    mp_handle_entry_t *entry = validate_handle(pool, handle, &err);
    if (!entry) return err;

    uint16_t vpn   = entry->start_vpn;
    uint16_t count = entry->page_count;
    uint16_t *v2p  = vpn2ppn_tbl(pool);
    mp_page_desc_t *pd = page_desc_tbl(pool);

    uint16_t first_ppn = v2p[vpn];
    if (first_ppn == MP_PPN_INVALID) {
        if (entry->delayed) return MP_ERR_NOT_LOCKED;
        if (pool->vm_enabled) return MP_ERR_NOT_LOCKED;
        return MP_ERR_NOT_LOCKED;
    }

    if (pd[first_ppn].lock_count == 0)
        return MP_ERR_NOT_LOCKED;

    for (uint16_t i = 1; i < count; i++) {
        uint16_t ppn = v2p[vpn + i];
        if (ppn == MP_PPN_INVALID || pd[ppn].lock_count == 0)
            return MP_ERR_NOT_LOCKED;
    }

    *out_ptr = 
    (uint8_t *)pool->pool_memory + (size_t)first_ppn * pool->page_size;
    return MP_OK;
}

/* ── mp_free_applicant ───────────────────────────────────────── */
mp_error_t mp_free_applicant_fn(mp_applicant_t *app, bool force, const char *file, int line) {
    (void)file; (void)line;
    if (!app) return MP_ERR_NULL_PTR;
    mp_pool_t *pool = app->pool;
    if (!pool) return MP_ERR_NULL_PTR;
    if (pool->metadata == NULL) return MP_ERR_NULL_PTR;
    mp_meta_header_t *mh = hdr(pool);
    if (mh->magic != MP_MAGIC) return MP_ERR_INVALID_POOL;

    mp_handle_entry_t *ht = handle_tbl(pool);
    mp_page_desc_t    *pd = page_desc_tbl(pool);
    uint16_t          *v2p = vpn2ppn_tbl(pool);

    if (!force) {
        for (uint16_t i = 0; i < mh->max_handles; i++) {
            if (!ht[i].allocated || ht[i].applicant_id != app->applicant_id)
                continue;
            uint16_t vpn   = ht[i].start_vpn;
            uint16_t count = ht[i].page_count;
            for (uint16_t p = 0; p < count; p++) {
                uint16_t ppn = v2p[vpn + p];
                if (ppn != MP_PPN_INVALID && pd[ppn].lock_count > 0)
                    return MP_ERR_INVALID_HANDLE;
            }
        }
    }

    for (uint16_t i = 0; i < mh->max_handles; i++) {
        if (!ht[i].allocated || ht[i].applicant_id != app->applicant_id)
            continue;
        mp_handle_t h = { .index = i, .generation = ht[i].generation };
        mp_error_t er = mp_free_fn(app, h, file, line);
        if (er != MP_OK) return er;
    }

    return MP_OK;
}
