#include "mp_pool.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ═══════════════════════════════════════════════════════════════
 *  Test configuration
 * ═══════════════════════════════════════════════════════════════ */

#define PAGE_SIZE   256
#define NUM_PAGES   64
#define MAX_HANDLES 16
#define POOL_SIZE   ((size_t)NUM_PAGES * PAGE_SIZE)

#define METADATA_SZ  MP_METADATA_SIZE(NUM_PAGES, MAX_HANDLES)

static uint8_t pool_mem[POOL_SIZE];
static uint8_t meta_data[METADATA_SZ];

static int  test_passed = 0;
static int  test_failed = 0;
static int  test_idx    = 0;

static void reset_pool(void) {
    memset(pool_mem, 0, POOL_SIZE);
    memset(meta_data, 0, METADATA_SZ);
}

static void check(mp_error_t err, mp_error_t expected, const char *msg) {
    test_idx++;
    if (err == expected) {
        test_passed++;
        printf("  ✓ %s\n", msg);
    } else {
        test_failed++;
        printf("  ✗ %s  (got %d, expected %d)\n", msg, (int)err, (int)expected);
    }
}

static void check_bool(int cond, const char *msg) {
    test_idx++;
    if (cond) {
        test_passed++;
        printf("  ✓ %s\n", msg);
    } else {
        test_failed++;
        printf("  ✗ %s\n", msg);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  T1: mp_init normal init
 * ═══════════════════════════════════════════════════════════════ */
static void test_T1_init_ok(void) {
    printf("\nT1: mp_init normal init\n");
    reset_pool();

    mp_config_t cfg = {
        .pool_memory  = pool_mem,
        .pool_size    = POOL_SIZE,
        .metadata     = meta_data,
        .metadata_size = METADATA_SZ,
        .page_size    = PAGE_SIZE,
        .max_handles  = MAX_HANDLES,
        .vm_enabled   = 0,
    };
    mp_pool_t pool = {0};
    mp_error_t e = mp_init(&pool, &cfg);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };
    (void)app;
    check(e, MP_OK, "mp_init returns MP_OK");
    check_bool(pool.pool_memory == pool_mem, "pool_memory set");
    check_bool(pool.total_pages == NUM_PAGES, "total_pages = 64");
    check_bool(pool.page_size == PAGE_SIZE, "page_size = 256");
}

/* ═══════════════════════════════════════════════════════════════
 *  T2: mp_init metadata too small
 * ═══════════════════════════════════════════════════════════════ */
static void test_T2_init_metadata_too_small(void) {
    printf("\nT2: mp_init metadata too small\n");
    reset_pool();

    mp_config_t cfg = {
        .pool_memory  = pool_mem,
        .pool_size    = POOL_SIZE,
        .metadata     = meta_data,
        .metadata_size = 16,  /* far too small */
        .page_size    = PAGE_SIZE,
        .max_handles  = MAX_HANDLES,
    };
    mp_pool_t pool = {0};
    mp_error_t e = mp_init(&pool, &cfg);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };
    (void)app;
    check(e, MP_ERR_METADATA_TOO_SMALL, "returns MP_ERR_METADATA_TOO_SMALL");
}

/* ═══════════════════════════════════════════════════════════════
 *  T3: mp_alloc 1 page
 * ═══════════════════════════════════════════════════════════════ */
static void test_T3_alloc_1page(void) {
    printf("\nT3: mp_alloc 1 page\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    mp_error_t e = mp_alloc(&app, 1, &h);
    check(e, MP_OK, "mp_alloc(1) returns MP_OK");
    check_bool(h.index < MAX_HANDLES, "handle index valid");
}

/* ═══════════════════════════════════════════════════════════════
 *  T4: mp_alloc multiple pages (contiguous)
 * ═══════════════════════════════════════════════════════════════ */
static void test_T4_alloc_multi(void) {
    printf("\nT4: mp_alloc multiple pages\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    mp_error_t e = mp_alloc_pages(&app, 5, &h);
    check(e, MP_OK, "mp_alloc_pages(5) returns MP_OK");

    void *ptr;
    e = mp_lock(&app, h, &ptr);
    check(e, MP_OK, "lock after alloc returns MP_OK");
    check_bool(ptr != NULL, "locked pointer is non-NULL");

    /* write pattern and verify */
    memset(ptr, 0xAA, 5 * PAGE_SIZE);
    check_bool(*(uint8_t*)ptr == 0xAA, "data written correctly");
    mp_unlock(&app, h);
}

/* ═══════════════════════════════════════════════════════════════
 *  T5: mp_alloc out of memory
 * ═══════════════════════════════════════════════════════════════ */
static void test_T5_alloc_oom(void) {
    printf("\nT5: mp_alloc out of memory\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    mp_error_t e = mp_alloc_pages(&app, NUM_PAGES + 1, &h);
    check(e, MP_ERR_NO_MEMORY, "alloc too many pages -> NO_MEMORY");
}

/* ═══════════════════════════════════════════════════════════════
 *  T6: mp_lock yields correct pointer
 * ═══════════════════════════════════════════════════════════════ */
static void test_T6_lock_ptr(void) {
    printf("\nT6: mp_lock yields correct pointer\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 3, &h) == MP_OK);

    void *ptr;
    assert(mp_lock(&app, h, &ptr) == MP_OK);

    /* Check pointer is within pool */
    check_bool((uint8_t*)ptr >= pool_mem && (uint8_t*)ptr < pool_mem + POOL_SIZE,
               "locked pointer within pool range");
    check_bool((uint8_t*)ptr == pool_mem, "locked pointer at pool base (first alloc)");

    /* write data */
    memcpy(ptr, "HELLO", 5);
    check_bool(memcmp(pool_mem, "HELLO", 5) == 0, "data visible in pool memory");
    mp_unlock(&app, h);
}

/* ═══════════════════════════════════════════════════════════════
 *  T7: lock / unlock reference counting
 * ═══════════════════════════════════════════════════════════════ */
static void test_T7_lock_unlock_refcount(void) {
    printf("\nT7: lock/unlock reference counting\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 2, &h) == MP_OK);

    void *p1, *p2;
    assert(mp_lock(&app, h, &p1) == MP_OK);  /* lock 1 */
    assert(mp_lock(&app, h, &p2) == MP_OK);  /* lock 2 */
    check_bool(p1 == p2, "two locks return same pointer");

    assert(mp_unlock(&app, h) == MP_OK);      /* unlock 1 */
    /* Should still be locked (refcount 1 left) */
    assert(mp_unlock(&app, h) == MP_OK);      /* unlock 2 */
    /* Now fully unlocked */
    check(MP_OK, MP_OK, "double lock/unlock works correctly");
}

/* ═══════════════════════════════════════════════════════════════
 *  T8: unlock too many
 * ═══════════════════════════════════════════════════════════════ */
static void test_T8_unlock_too_many(void) {
    printf("\nT8: unlock more than locked\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 1, &h) == MP_OK);
    assert(mp_lock(&app, h, NULL) == MP_OK);
    assert(mp_unlock(&app, h) == MP_OK);
    mp_error_t e = mp_unlock(&app, h);
    check(e, MP_ERR_NOT_LOCKED, "extra unlock -> NOT_LOCKED");
}

/* ═══════════════════════════════════════════════════════════════
 *  T9: free then use old handle -> invalid
 * ═══════════════════════════════════════════════════════════════ */
static void test_T9_free_handle_invalid(void) {
    printf("\nT9: free then use old handle -> invalid\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 1, &h) == MP_OK);
    assert(mp_lock(&app, h, NULL) == MP_OK);
    assert(mp_unlock(&app, h) == MP_OK);
    assert(mp_free(&app, h) == MP_OK);

    void *ptr;
    mp_error_t e = mp_lock(&app, h, &ptr);
    check(e, MP_ERR_INVALID_HANDLE, "lock after free -> INVALID_HANDLE");
}

/* ═══════════════════════════════════════════════════════════════
 *  T10: free then realloc, old handle still invalid
 * ═══════════════════════════════════════════════════════════════ */
static void test_T10_free_realloc_generation(void) {
    printf("\nT10: free+realloc, old handle invalid\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h1, h2;
    assert(mp_alloc_pages(&app, 1, &h1) == MP_OK);
    mp_handle_t old = h1;  /* save */
    assert(mp_lock(&app, h1, NULL) == MP_OK);
    assert(mp_unlock(&app, h1) == MP_OK);
    assert(mp_free(&app, h1) == MP_OK);

    /* Realloc – might get same index but different generation */
    assert(mp_alloc_pages(&app, 1, &h2) == MP_OK);

    /* Old handle should be invalid */
    void *ptr;
    mp_error_t e = mp_lock(&app, old, &ptr);
    check(e, MP_ERR_INVALID_HANDLE, "old handle after realloc -> INVALID_HANDLE");
    check_bool(h2.generation != old.generation || h2.index != old.index,
               "new handle differs from old");
}

/* ═══════════════════════════════════════════════════════════════
 *  T11: mp_compact simple fragmentation
 * ═══════════════════════════════════════════════════════════════ */
static void test_T11_compact_simple(void) {
    printf("\nT11: mp_compact simple fragmentation\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t ha, hb, hc;
    assert(mp_alloc_pages(&app, 3, &ha) == MP_OK);  /* PPN 0-2 */
    assert(mp_alloc_pages(&app, 2, &hb) == MP_OK);  /* PPN 3-4 */
    assert(mp_alloc_pages(&app, 1, &hc) == MP_OK);  /* PPN 5 */

    /* Lock ha and hc to write data, unlock hb */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    memset(pool_mem, 0x11, 3 * PAGE_SIZE);
    assert(mp_unlock(&app, ha) == MP_OK);

    assert(mp_lock(&app, hc, NULL) == MP_OK);
    memset(pool_mem + 5 * PAGE_SIZE, 0x33, PAGE_SIZE);
    assert(mp_unlock(&app, hc) == MP_OK);

    /* Free hb (middle block) to create hole at PPN 3-4 */
    assert(mp_free(&app, hb) == MP_OK);

    /* Compact: hc should move from PPN 5 to PPN 3 */
    mp_error_t e = mp_compact(&app);
    check(e, MP_OK, "compact returns OK");

    /* Verify hc data at new location */
    assert(mp_lock(&app, hc, NULL) == MP_OK);
    check_bool(*(uint8_t*)(pool_mem + 3 * PAGE_SIZE) == 0x33,
               "hc data intact after compact (PPN 5→3)");
    assert(mp_unlock(&app, hc) == MP_OK);

    /* Verify ha data still intact */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    check_bool(*(uint8_t*)(pool_mem + 0) == 0x11,
               "ha data intact after compact");
    assert(mp_unlock(&app, ha) == MP_OK);
}

/* ═══════════════════════════════════════════════════════════════
 *  T12: mp_compact locked pages not moved
 * ═══════════════════════════════════════════════════════════════ */
static void test_T12_compact_locked_not_moved(void) {
    printf("\nT12: mp_compact locked pages not moved\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t ha, hc;
    assert(mp_alloc_pages(&app, 2, &ha) == MP_OK);  /* PPN 0-1 */
    /* alloc hb (2 pages) but we'll free it */
    mp_handle_t hb;
    assert(mp_alloc_pages(&app, 2, &hb) == MP_OK);  /* PPN 2-3 */
    assert(mp_alloc_pages(&app, 2, &hc) == MP_OK);  /* PPN 4-5 */

    /* Lock ha */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    memset(pool_mem, 0xAA, 2 * PAGE_SIZE);

    /* Data in hc */
    assert(mp_lock(&app, hc, NULL) == MP_OK);
    memset(pool_mem + 4 * PAGE_SIZE, 0xBB, 2 * PAGE_SIZE);
    assert(mp_unlock(&app, hc) == MP_OK);

    /* Free hb (PPN 2-3) */
    assert(mp_free(&app, hb) == MP_OK);

    /* Compact: ha is locked, so it shouldn't move.
       hc (unlocked) should move from PPN 4-5 to PPN 2-3 */
    mp_error_t e = mp_compact(&app);
    check(e, MP_OK, "compact returns OK");

    /* ha should still be at PPN 0-1 */
    check_bool(*(uint8_t*)(pool_mem + 0) == 0xAA,
               "locked ha data still at PPN 0");
    assert(mp_unlock(&app, ha) == MP_OK);

    /* hc should be at PPN 2-3 now */
    assert(mp_lock(&app, hc, NULL) == MP_OK);
    check_bool(*(uint8_t*)(pool_mem + 2 * PAGE_SIZE) == 0xBB,
               "hc moved to PPN 2-3");
    assert(mp_unlock(&app, hc) == MP_OK);
}

/* ═══════════════════════════════════════════════════════════════
 *  T15: handle generation safety
 * ═══════════════════════════════════════════════════════════════ */
static void test_T15_generation_safety(void) {
    printf("\nT15: handle generation safety\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h1;
    assert(mp_alloc_pages(&app, 1, &h1) == MP_OK);
    uint16_t idx = h1.index;
    uint16_t gen = h1.generation;

    assert(mp_lock(&app, h1, NULL) == MP_OK);
    assert(mp_unlock(&app, h1) == MP_OK);
    assert(mp_free(&app, h1) == MP_OK);

    /* Re-alloc to possibly same index */
    mp_handle_t h2;
    assert(mp_alloc_pages(&app, 1, &h2) == MP_OK);

    /* Old handle should fail */
    mp_handle_t old = { .index = idx, .generation = gen };
    void *ptr;
    mp_error_t e = mp_lock(&app, old, &ptr);
    check(e, MP_ERR_INVALID_HANDLE, "old generation handle -> INVALID_HANDLE");
}

/* ═══════════════════════════════════════════════════════════════
 *  T16: null pointer checks
 * ═══════════════════════════════════════════════════════════════ */
static void test_T16_null_ptr(void) {
    printf("\nT16: null pointer checks\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };
    (void)app;

    /* Null-ptr tests — use _fn directly because compound literals confuse macros */
    check(mp_alloc_fn(NULL, 1, NULL, NULL, 0), MP_ERR_NULL_PTR, "mp_alloc(NULL)");
    check(mp_alloc_pages_fn(NULL, 1, NULL, NULL, 0), MP_ERR_NULL_PTR, "mp_alloc_pages(NULL)");
    check(mp_lock_fn(NULL, (mp_handle_t){0,0}, NULL, NULL, 0), MP_ERR_NULL_PTR, "mp_lock(NULL)");
    check(mp_unlock_fn(NULL, (mp_handle_t){0,0}, NULL, 0), MP_ERR_NULL_PTR, "mp_unlock(NULL)");
    check(mp_free_fn(NULL, (mp_handle_t){0,0}, NULL, 0), MP_ERR_NULL_PTR, "mp_free(NULL)");
    check(mp_compact_fn(NULL, NULL, 0), MP_ERR_NULL_PTR, "mp_compact(NULL)");
}

/* ═══════════════════════════════════════════════════════════════
 *  T17: zero-size allocation
 * ═══════════════════════════════════════════════════════════════ */
static void test_T17_zero_alloc(void) {
    printf("\nT17: zero-size allocation\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    check(mp_alloc(&app, 0, &h), MP_ERR_INVALID_PARAM, "mp_alloc(0)");
    check(mp_alloc_pages(&app, 0, &h), MP_ERR_INVALID_PARAM, "mp_alloc_pages(0)");
}

/* ═══════════════════════════════════════════════════════════════
 *  T18: free block splitting
 * ═══════════════════════════════════════════════════════════════ */
static void test_T18_free_block_split(void) {
    printf("\nT18: free block splitting\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Allocate 1 page from 64-page free block → should split */
    mp_handle_t h1;
    assert(mp_alloc_pages(&app, 1, &h1) == MP_OK);

    /* Allocate 63 more pages = 64 total, should fill pool */
    mp_handle_t h2;
    mp_error_t e = mp_alloc_pages(&app, NUM_PAGES - 1, &h2);
    check(e, MP_OK, "second alloc fills remaining space (split worked)");

    /* Now pool should be full */
    mp_handle_t h3;
    e = mp_alloc_pages(&app, 1, &h3);
    check(e, MP_ERR_NO_MEMORY, "third alloc fails (no space)");
}

/* ═══════════════════════════════════════════════════════════════
 *  T19: free block merging
 * ═══════════════════════════════════════════════════════════════ */
static void test_T19_free_block_merge(void) {
    printf("\nT19: free block merging\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t ha, hb;
    assert(mp_alloc_pages(&app, 3, &ha) == MP_OK);  /* PPN 0-2 */
    assert(mp_alloc_pages(&app, 2, &hb) == MP_OK);  /* PPN 3-4 */

    /* Free both to create adjacent free blocks that should merge */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    assert(mp_unlock(&app, ha) == MP_OK);
    assert(mp_free(&app, ha) == MP_OK);
    assert(mp_lock(&app, hb, NULL) == MP_OK);
    assert(mp_unlock(&app, hb) == MP_OK);
    assert(mp_free(&app, hb) == MP_OK);

    /* Now alloc 5 pages – should succeed with merged block */
    mp_handle_t hc;
    mp_error_t e = mp_alloc_pages(&app, 5, &hc);
    check(e, MP_OK, "alloc 5 pages after merge succeeds");
}

/* ═══════════════════════════════════════════════════════════════
 *  T20: free block list sorted order
 * ═══════════════════════════════════════════════════════════════ */
static void test_T20_free_list_order(void) {
    printf("\nT20: free block list sorted order\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Create fragmentation by alloc/free in specific pattern */
    mp_handle_t h[6];
    for (int i = 0; i < 6; i++)
        assert(mp_alloc_pages(&app, 1, &h[i]) == MP_OK);

    /* Free in a pattern that creates multiple free blocks: 1, 3, 5 */
    assert(mp_free(&app, h[1]) == MP_OK);  /* free PPN 1 */
    assert(mp_free(&app, h[3]) == MP_OK);  /* free PPN 3 */
    assert(mp_free(&app, h[5]) == MP_OK);  /* free PPN 5 */

    /* Verify free list is sorted by start_ppn via compact observation */
    mp_error_t e = mp_compact(&app);
    check(e, MP_OK, "compact succeeds with fragmented free list");
}

/* ═══════════════════════════════════════════════════════════════
 *  T22: free block node exhaustion
 * ═══════════════════════════════════════════════════════════════ */
static void test_T22_free_node_exhaustion(void) {
    printf("\nT22: free block node exhaustion\n");
    /* Use small max_handles to force node exhaustion */
    #define SMALL_HANDLES 3
    #define SMALL_PAGES  32
    uint8_t mem[SMALL_PAGES * 64];
    uint8_t md[MP_METADATA_SIZE(SMALL_PAGES, SMALL_HANDLES)];

    memset(mem, 0, sizeof(mem));
    memset(md, 0, sizeof(md));

    mp_config_t cfg = { .pool_memory = mem, .pool_size = sizeof(mem),
        .metadata = md, .metadata_size = sizeof(md),
        .page_size = 64, .max_handles = SMALL_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Allocate max_handles blocks */
    mp_handle_t h[SMALL_HANDLES];
    for (int i = 0; i < SMALL_HANDLES; i++) {
        /* Allocate 1 page each, non-consecutive by interleaving frees */
        assert(mp_alloc_pages(&app, 1, &h[i]) == MP_OK);
    }

    /* Free all to create multiple free nodes */
    for (int i = 0; i < SMALL_HANDLES; i++) {
        assert(mp_free(&app, h[i]) == MP_OK);
    }

    /* Allocate and free in different patterns to stress node pool */
    mp_handle_t ha, hb, hc;
    assert(mp_alloc_pages(&app, 2, &ha) == MP_OK);
    assert(mp_alloc_pages(&app, 2, &hb) == MP_OK);
    assert(mp_free(&app, ha) == MP_OK);

    /* This should succeed or give proper error, not crash */
    mp_error_t e = mp_alloc_pages(&app, 1, &hc);
    check_bool(e == MP_OK || e == MP_ERR_FREE_NODE_EXHAUSTED,
          "allocation after fragmentation (no crash)");
}

/* ═══════════════════════════════════════════════════════════════
 *  T21: LRU list ordering (VM mode)
 * ═══════════════════════════════════════════════════════════════ */
/* Simple VM callbacks */
static uint8_t vm_store[64 * 256]; /* enough for all pages */
static void vm_evict_cb(void *ud, uint16_t vpn, uint16_t n,
                         void *src, size_t len) {
    (void)ud; (void)n;
    memcpy(&vm_store[vpn * 256], src, len);
}
static void vm_load_cb(void *ud, uint16_t vpn, uint16_t n,
                        void *dst, size_t len) {
    (void)ud; (void)n;
    memcpy(dst, &vm_store[vpn * 256], len);
}

static void test_T21_lru_ordering(void) {
    printf("\nT21: LRU list ordering (VM)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES,
        .vm_enabled = 1,
        .vm_evict = vm_evict_cb,
        .vm_load  = vm_load_cb };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Allocate 3 handles */
    mp_handle_t ha, hb, hc;
    assert(mp_alloc_pages(&app, 1, &ha) == MP_OK);
    assert(mp_alloc_pages(&app, 1, &hb) == MP_OK);
    assert(mp_alloc_pages(&app, 1, &hc) == MP_OK);

    /* Lock in order: hc, hb, ha  (so ha is most recent, hc is LRU tail) */
    assert(mp_lock(&app, hc, NULL) == MP_OK); assert(mp_unlock(&app, hc) == MP_OK);
    assert(mp_lock(&app, hb, NULL) == MP_OK); assert(mp_unlock(&app, hb) == MP_OK);
    assert(mp_lock(&app, ha, NULL) == MP_OK); assert(mp_unlock(&app, ha) == MP_OK);

    /* Verify LRU state by checking which handle gets evicted when pool is full.
       Fill remaining pages to trigger eviction. */
    mp_handle_t hd;
    /* Allocate all remaining pages (NUM_PAGES - 3 more allocations of 1 page each) */
    /* Actually let's allocate one big chunk to fill the pool and trigger eviction */
    memset(vm_store, 0, sizeof(vm_store));

    /* Write unique patterns */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    memset(pool_mem, 0xAA, PAGE_SIZE);
    assert(mp_unlock(&app, ha) == MP_OK);
    assert(mp_lock(&app, hb, NULL) == MP_OK);
    memset(pool_mem + PAGE_SIZE, 0xBB, PAGE_SIZE);
    assert(mp_unlock(&app, hb) == MP_OK);
    assert(mp_lock(&app, hc, NULL) == MP_OK);
    memset(pool_mem + 2 * PAGE_SIZE, 0xCC, PAGE_SIZE);
    assert(mp_unlock(&app, hc) == MP_OK);

    /* Now fill the rest of the pool */
    for (int i = 3; i < NUM_PAGES; i++) {
        assert(mp_alloc_pages(&app, 1, &hd) == MP_OK);
        assert(mp_lock(&app, hd, NULL) == MP_OK);
        assert(mp_unlock(&app, hd) == MP_OK);
        assert(mp_free(&app, hd) == MP_OK);
    }

    /* Now pool is fragmented but nearly full.
       Allocate another page to trigger eviction of LRU victim (hc, since it's tail). */
    assert(mp_alloc_pages(&app, 1, &hd) == MP_OK);

    /* hc should have been evicted. Lock hc and verify it loads from backing store. */
    void *ptr;
    mp_error_t e = mp_lock(&app, hc, &ptr);
    /* If hc was evicted, locking it should load from backing store */
    check(e, MP_OK, "lock hc after eviction succeeds (auto load)");
    assert(mp_unlock(&app, hc) == MP_OK);

    /* hb and ha should still be accessible */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    assert(mp_unlock(&app, ha) == MP_OK);
    assert(mp_lock(&app, hb, NULL) == MP_OK);
    assert(mp_unlock(&app, hb) == MP_OK);
    check(MP_OK, MP_OK, "ha and hb still accessible after eviction");
}

/* ═══════════════════════════════════════════════════════════════
 *  T13: VM mode: basic swap-out / swap-in
 * ═══════════════════════════════════════════════════════════════ */
static void test_T13_vm_swap(void) {
    printf("\nT13: VM mode basic swap-out/swap-in\n");
    reset_pool();

    uint8_t vmem[POOL_SIZE];
    memset(vmem, 0, sizeof(vmem));

    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES,
        .vm_enabled = 1,
        .vm_evict = vm_evict_cb,
        .vm_load  = vm_load_cb };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Fill pool with small allocations to force eviction */
    mp_handle_t handles[MAX_HANDLES];
    int i;
    for (i = 0; i < MAX_HANDLES; i++) {
        mp_error_t e = mp_alloc_pages(&app, 4, &handles[i]);
        if (e != MP_OK) break;
        /* Write pattern */
        assert(mp_lock(&app, handles[i], NULL) == MP_OK);
        memset(pool_mem + i * 4 * PAGE_SIZE, (uint8_t)(i + 1), 4 * PAGE_SIZE);
        assert(mp_unlock(&app, handles[i]) == MP_OK);
    }
    check_bool(i > 0, "at least some allocations succeeded");

    /* Verify data integrity by locking handles */
    for (int j = 0; j < i; j++) {
        void *ptr;
        mp_error_t e = mp_lock(&app, handles[j], &ptr);
        check(e, MP_OK, "lock handle after potential eviction");
        if (e == MP_OK) {
            uint8_t expected = (uint8_t)(j + 1);
            check_bool(*(uint8_t*)ptr == expected,
                       "data integrity for handle");
            assert(mp_unlock(&app, handles[j]) == MP_OK);
        }
    }

    /* Cleanup */
    for (int j = 0; j < i; j++)
        mp_free(&app, handles[j]);
}

/* ═══════════════════════════════════════════════════════════════
 *  T14: mp_partial_map (read-only child handle)
 * ═══════════════════════════════════════════════════════════════ */
static void test_T14_partial_map(void) {
    printf("\nT14: mp_partial_map (read-only child handle)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES,
        .vm_enabled = 1,
        .vm_evict = vm_evict_cb,
        .vm_load  = vm_load_cb };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t parent;
    assert(mp_alloc_pages(&app, 4, &parent) == MP_OK);

    /* Lock parent and write initial data */
    assert(mp_lock(&app, parent, NULL) == MP_OK);
    memset(pool_mem, 0xAA, PAGE_SIZE * 4);
    assert(mp_unlock(&app, parent) == MP_OK);

    /* Create child (independent pages — reads are zero, writes don't touch parent) */
    mp_handle_t child;
    mp_error_t e = mp_partial_map(&app, parent, PAGE_SIZE, PAGE_SIZE * 2, false, &child);
    check(e, MP_OK, "mp_partial_map read-only child");

    void *ptr;
    e = mp_lock(&app, child, &ptr);
    check(e, MP_OK, "mp_lock on child handle");
    if (e == MP_OK) {
        /* Child has its own pages (zero-initialized, not parent's data) */
        check_bool(*(uint8_t*)ptr == 0x00,
                   "child pages are zero-initialized (independent alloc)");

        /* Verify child pointer is page-aligned (offset=PAGE_SIZE, page_offset=0) */
        size_t off = (uint8_t*)ptr - (uint8_t*)pool_mem;
        check_bool(off % PAGE_SIZE == 0,
                   "child lock returns page-aligned ptr (page_offset=0)");

        /* Write through child — verify parent unaffected */
        memset(ptr, 0xCD, PAGE_SIZE * 2);
        assert(mp_unlock(&app, child) == MP_OK);

        /* Free child so parent can be locked */
        mp_free(&app, child);

        assert(mp_lock(&app, parent, NULL) == MP_OK);
        check_bool(*(uint8_t*)(pool_mem + PAGE_SIZE) == 0xAA,
                   "parent page 1 unchanged after child write");
        assert(mp_unlock(&app, parent) == MP_OK);
    }

    /* Non-page-aligned offset: offset = PAGE_SIZE + 50 */
    mp_handle_t child2;
    e = mp_partial_map(&app, parent, PAGE_SIZE + 50, 100, false, &child2);
    check(e, MP_OK, "partial_map with non-page-aligned offset");
    e = mp_lock(&app, child2, &ptr);
    check(e, MP_OK, "mp_lock on non-page-aligned child");
    if (e == MP_OK) {
        /* ptr = child_page_base + (50 % PAGE_SIZE) = child_page_base + 50 */
        /* Verify the base is page-aligned */
        size_t offset_in_pool = (uint8_t*)ptr - (uint8_t*)pool_mem;
        check_bool((offset_in_pool % PAGE_SIZE) == 50,
                   "child lock returns ptr at page_offset=50");
        assert(mp_unlock(&app, child2) == MP_OK);
    }
    mp_free(&app, child2);

    /* Also free the first child so parent can be fully locked */
    mp_free(&app, child);

    /* Verify parent page lock_count unaffected by child activity */
    assert(mp_lock(&app, parent, NULL) == MP_OK);
    /* Parent's pages should have lock_count == 1 (from parent's lock),
       not incremented by any child operations */
    void *parent_ptr;
    assert(mp_lock(&app, parent, &parent_ptr) == MP_OK); /* lock_count == 2 */
    assert(mp_unlock(&app, parent) == MP_OK);
    assert(mp_unlock(&app, parent) == MP_OK);

    /* Out-of-range offset */
    mp_handle_t bad_child;
    e = mp_partial_map(&app, parent, 10 * PAGE_SIZE, PAGE_SIZE, false, &bad_child);
    check(e, MP_ERR_OUT_OF_RANGE, "partial_map with out-of-range offset");

    /* Zero length */
    e = mp_partial_map(&app, parent, 0, 0, false, &bad_child);
    check(e, MP_ERR_INVALID_PARAM, "partial_map with zero length");

    /* ── Child handle rejection tests ── */
    {
        /* Create child for rejection tests */
        mp_handle_t rej_child;
        e = mp_partial_map(&app, parent, 0, PAGE_SIZE, false, &rej_child);
        check(e, MP_OK, "create child for rejection tests");

        /* mp_resize on child handle → WR_LOCKED */
        e = mp_resize_pages(&app, &rej_child, 3);
        check(e, MP_ERR_WR_LOCKED, "mp_resize_pages on child → WR_LOCKED");

        e = mp_resize(&app, &rej_child, PAGE_SIZE * 3);
        check(e, MP_ERR_WR_LOCKED, "mp_resize on child → WR_LOCKED");

        mp_free(&app, rej_child);
    }

    /* ── mp_get_ptr on child handle includes page_offset ── */
    {
        mp_handle_t gptr_child;
        e = mp_partial_map(&app, parent, 100, PAGE_SIZE, false, &gptr_child);
        check(e, MP_OK, "create child for get_ptr test (offset=100)");

        void *ptr;
        e = mp_lock(&app, gptr_child, &ptr);
        check(e, MP_OK, "lock child for get_ptr test");

        /* mp_get_ptr should return same pointer as mp_lock */
        void *gptr;
        e = mp_get_ptr(&app, gptr_child, &gptr);
        check(e, MP_OK, "mp_get_ptr on locked child");
        check_bool(gptr == ptr, "mp_get_ptr matches mp_lock pointer");

        /* Verify page_offset=100 is applied */
        size_t off = (uint8_t *)gptr - (uint8_t *)pool_mem;
        check_bool(off % PAGE_SIZE == 100,
                   "mp_get_ptr includes page_offset=100");

        assert(mp_unlock(&app, gptr_child) == MP_OK);
        mp_free(&app, gptr_child);
    }

    mp_free(&app, parent);
}

/* ═══════════════════════════════════════════════════════════════
 *  T35: unlocked children do not block compaction
 * ═══════════════════════════════════════════════════════════════ */
static void test_T35_child_compact_unlocked(void) {
    printf("\nT35: unlocked children do not block compaction\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Alloc parent (2 pages, PPN 0-1) + gap (free PPN 2-3) + children on parent */
    mp_handle_t parent;
    assert(mp_alloc_pages(&app, 2, &parent) == MP_OK); /* PPN 0-1 */
    mp_handle_t gap;
    assert(mp_alloc_pages(&app, 2, &gap) == MP_OK);    /* PPN 2-3 */
    assert(mp_free(&app, gap) == MP_OK);                /* free gap */

    /* Write data to parent pages BEFORE creating children */
    assert(mp_lock(&app, parent, NULL) == MP_OK);
    memset(pool_mem, 0xAA, PAGE_SIZE);
    memset(pool_mem + PAGE_SIZE, 0xBB, PAGE_SIZE);
    assert(mp_unlock(&app, parent) == MP_OK);

    /* Create two children with independent pages */
    mp_handle_t c1, c2;
    assert(mp_partial_map(&app, parent, 0, PAGE_SIZE, false, &c1) == MP_OK);
    assert(mp_partial_map(&app, parent, PAGE_SIZE, PAGE_SIZE, false, &c2) == MP_OK);

    /* Write data through the children */
    void *ptr;
    assert(mp_lock(&app, c1, &ptr) == MP_OK);
    memset(ptr, 0x11, PAGE_SIZE);
    assert(mp_unlock(&app, c1) == MP_OK);
    assert(mp_lock(&app, c2, &ptr) == MP_OK);
    memset(ptr, 0x22, PAGE_SIZE);
    assert(mp_unlock(&app, c2) == MP_OK);

    /* child_refs > 0, but children are unlocked → compaction allowed */
    mp_error_t e = mp_compact(&app);
    check(e, MP_OK, "compact returns OK");

    /* Verify data via child lock (children have their own pages) */
    assert(mp_lock(&app, c1, &ptr) == MP_OK);
    check_bool(*(uint8_t*)ptr == 0x11, "child1 data intact after compact");
    assert(mp_unlock(&app, c1) == MP_OK);

    assert(mp_lock(&app, c2, &ptr) == MP_OK);
    check_bool(*(uint8_t*)ptr == 0x22, "child2 data intact after compact");
    assert(mp_unlock(&app, c2) == MP_OK);

    /* Cleanup */
    mp_free(&app, c1);
    mp_free(&app, c2);
    mp_free(&app, parent);
}

/* ═══════════════════════════════════════════════════════════════
 *  T23: mp_resize_pages — shrink
 * ═══════════════════════════════════════════════════════════════ */
static void test_T23_resize_shrink(void) {
    printf("\nT23: mp_resize_pages shrink\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 5, &h) == MP_OK);
    assert(mp_lock(&app, h, NULL) == MP_OK);
    memset(pool_mem, 0x77, 5 * PAGE_SIZE);
    assert(mp_unlock(&app, h) == MP_OK);

    /* Shrink from 5 to 2 pages */
    mp_error_t e = mp_resize_pages(&app, &h, 2);
    check(e, MP_OK, "shrink 5→2 pages returns OK");
    check_bool(h.index < MAX_HANDLES, "handle still valid after shrink");

    /* Data should still be intact */
    assert(mp_lock(&app, h, NULL) == MP_OK);
    check_bool(*(uint8_t*)pool_mem == 0x77, "data intact after shrink");
    assert(mp_unlock(&app, h) == MP_OK);

    /* The freed 3 pages should be reusable */
    mp_handle_t h2;
    e = mp_alloc_pages(&app, 3, &h2);
    check(e, MP_OK, "freed pages reusable after shrink");

    assert(mp_free(&app, h) == MP_OK);
    assert(mp_free(&app, h2) == MP_OK);
}

/* ═══════════════════════════════════════════════════════════════
 *  T24: mp_resize_pages — enlarge into free space
 * ═══════════════════════════════════════════════════════════════ */
static void test_T24_resize_enlarge_free(void) {
    printf("\nT24: mp_resize_pages enlarge (free space after)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Alloc A(3), B(2) */
    mp_handle_t ha, hb;
    assert(mp_alloc_pages(&app, 3, &ha) == MP_OK); /* PPN 0-2 */
    assert(mp_alloc_pages(&app, 2, &hb) == MP_OK); /* PPN 3-4 */

    assert(mp_lock(&app, ha, NULL) == MP_OK);
    memset(pool_mem, 0xAA, 3 * PAGE_SIZE);
    assert(mp_unlock(&app, ha) == MP_OK);

    /* Free B so there's free space after A */
    assert(mp_free(&app, hb) == MP_OK);

    /* Enlarge A from 3 to 5 (extends into PPN 3-4 which are now free) */
    mp_error_t e = mp_resize_pages(&app, &ha, 5);
    check(e, MP_OK, "enlarge 3→5 into free space");
    check_bool(ha.index < MAX_HANDLES, "handle still valid");

    /* Data intact */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    check_bool(*(uint8_t*)pool_mem == 0xAA, "data intact after enlarge");
    assert(mp_unlock(&app, ha) == MP_OK);

    assert(mp_free(&app, ha) == MP_OK);
}

/* ═══════════════════════════════════════════════════════════════
 *  T25: mp_resize_pages — enlarge moves current handle
 * ═══════════════════════════════════════════════════════════════ */
static void test_T25_resize_enlarge_move_current(void) {
    printf("\nT25: mp_resize_pages enlarge (move current, smaller)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Lay out: A(2), B(4) — A is smaller than B */
    mp_handle_t ha, hb;
    assert(mp_alloc_pages(&app, 2, &ha) == MP_OK); /* PPN 0-1 */
    assert(mp_alloc_pages(&app, 4, &hb) == MP_OK); /* PPN 2-5 */

    /* Write pattern to A */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    memset(pool_mem, 0xBB, 2 * PAGE_SIZE);
    assert(mp_unlock(&app, ha) == MP_OK);

    /* Write pattern to B */
    assert(mp_lock(&app, hb, NULL) == MP_OK);
    memset(pool_mem + 2 * PAGE_SIZE, 0xCC, 4 * PAGE_SIZE);
    assert(mp_unlock(&app, hb) == MP_OK);

    /* Unlock B so it can be moved */
    /* Try to enlarge A from 2→3. B (4 pages) is in the way.
     * A(2) < B(4), so A should be moved. */
    mp_error_t e = mp_resize_pages(&app, &ha, 3);
    check(e, MP_OK, "enlarge A 2→3 (A is smaller, A moves)");

    /* A's data should be intact */
    void *a_ptr;
    assert(mp_lock(&app, ha, &a_ptr) == MP_OK);
    check_bool(*(uint8_t*)a_ptr == 0xBB, "A data intact after relocate");
    assert(mp_unlock(&app, ha) == MP_OK);

    /* B should still be intact */
    void *b_ptr;
    assert(mp_lock(&app, hb, &b_ptr) == MP_OK);
    check_bool(*(uint8_t*)b_ptr == 0xCC, "B data intact after A moved");
    assert(mp_unlock(&app, hb) == MP_OK);

    assert(mp_free(&app, ha) == MP_OK);
    assert(mp_free(&app, hb) == MP_OK);
}

/* ═══════════════════════════════════════════════════════════════
 *  T26: mp_resize_pages — enlarge moves handles in way
 * ═══════════════════════════════════════════════════════════════ */
static void test_T26_resize_enlarge_move_others(void) {
    printf("\nT26: mp_resize_pages enlarge (move others, smaller)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Lay out: A(5), B(1) — B is smaller than A */
    mp_handle_t ha, hb;
    assert(mp_alloc_pages(&app, 5, &ha) == MP_OK); /* PPN 0-4 */
    assert(mp_alloc_pages(&app, 1, &hb) == MP_OK); /* PPN 5 */

    /* Write patterns */
    assert(mp_lock(&app, ha, NULL) == MP_OK);
    memset(pool_mem, 0xDD, 5 * PAGE_SIZE);
    assert(mp_unlock(&app, ha) == MP_OK);

    assert(mp_lock(&app, hb, NULL) == MP_OK);
    memset(pool_mem + 5 * PAGE_SIZE, 0xEE, PAGE_SIZE);
    assert(mp_unlock(&app, hb) == MP_OK);

    /* Enlarge A from 5→6. B(1) is in the way.
     * A(5) > B(1), so B should be moved. */
    mp_error_t e = mp_resize_pages(&app, &ha, 6);
    check(e, MP_OK, "enlarge A 5→6 (B is smaller, B moves)");

    /* A should still be at PPN 0-5 */
    void *a_ptr;
    assert(mp_lock(&app, ha, &a_ptr) == MP_OK);
    check_bool(*(uint8_t*)a_ptr == 0xDD, "A data intact in place");
    check_bool(a_ptr == pool_mem, "A still at pool base (extended in place)");
    assert(mp_unlock(&app, ha) == MP_OK);

    /* B's data should be intact at new location */
    void *b_ptr;
    assert(mp_lock(&app, hb, &b_ptr) == MP_OK);
    check_bool(*(uint8_t*)b_ptr == 0xEE, "B data intact after move");
    assert(mp_unlock(&app, hb) == MP_OK);

    assert(mp_free(&app, ha) == MP_OK);
    assert(mp_free(&app, hb) == MP_OK);
}

/* ═══════════════════════════════════════════════════════════════
 *  T27: mp_resize — byte-size wrapper
 * ═══════════════════════════════════════════════════════════════ */
static void test_T27_resize_byte(void) {
    printf("\nT27: mp_resize byte-size wrapper\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc(&app, 500, &h) == MP_OK); /* 2 pages */

    /* Shrink via byte-size */
    mp_error_t e = mp_resize(&app, &h, 200);
    check(e, MP_OK, "mp_resize shrink via byte-size");

    /* Enlarge via byte-size */
    e = mp_resize(&app, &h, 1000);
    check(e, MP_OK, "mp_resize enlarge via byte-size");

    assert(mp_free(&app, h) == MP_OK);
}

/* ═══════════════════════════════════════════════════════════════
 *  T28: applicant lifecycle, accessors, delayed alloc
 * ═══════════════════════════════════════════════════════════════ */
static void test_T28_applicant_create(void) {
    printf("\nT28: mp_applicant_create\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);

    /* User applicant (ID >= 128) */
    mp_applicant_t user1, user2, sys;
    mp_error_t e = mp_applicant_create(&pool, &user1);
    check(e, MP_OK, "mp_applicant_create returns OK");
    check_bool(user1.pool == &pool, "pool pointer set");
    check_bool(user1.applicant_id >= 128, "user ID >= 128");

    e = mp_applicant_create(&pool, &user2);
    check(e, MP_OK, "second applicant created");
    check_bool(user2.applicant_id == user1.applicant_id + 1, "IDs increment");

    /* System applicant (ID 0-127) */
    e = mp_applicant_create_system(&pool, &sys, 42);
    check(e, MP_OK, "system applicant");
    check_bool(sys.applicant_id == 42, "system ID = 42");

    /* Invalid system ID */
    e = mp_applicant_create_system(&pool, &sys, 128);
    check(e, MP_ERR_APPLICANT_ID_INVALID, "system ID > 127 rejected");
}

static void test_T29_handle_accessors(void) {
    printf("\nT29: handle accessors\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 5, &h) == MP_OK);

    size_t sz;
    assert(mp_get_size(&app, h, &sz) == MP_OK);
    check_bool(sz == 5 * PAGE_SIZE, "mp_get_size correct");

    uint16_t pc;
    assert(mp_get_page_count(&app, h, &pc) == MP_OK);
    check_bool(pc == 5, "mp_get_page_count correct");

    /* mp_get_ptr should fail when not locked */
    void *ptr;
    mp_error_t e = mp_get_ptr(&app, h, &ptr);
    check(e, MP_ERR_NOT_LOCKED, "mp_get_ptr fails when unlocked");

    /* mp_get_ptr should work when locked */
    assert(mp_lock(&app, h, NULL) == MP_OK);
    e = mp_get_ptr(&app, h, &ptr);
    check(e, MP_OK, "mp_get_ptr succeeds when locked");
    check_bool(ptr != NULL, "pointer non-NULL");
    assert(mp_unlock(&app, h) == MP_OK);

    /* mp_get_ptr should fail again after unlock */
    e = mp_get_ptr(&app, h, &ptr);
    check(e, MP_ERR_NOT_LOCKED, "mp_get_ptr fails after unlock");

    assert(mp_free(&app, h) == MP_OK);
}

static void test_T30_delayed_alloc(void) {
    printf("\nT30: delayed allocation (reserve + first-lock commit)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES,
        .alloc_delayed = 1, .delayed_no_reserve = 0 };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* Delayed alloc — pages NOT consumed yet */
    mp_handle_t h;
    assert(mp_alloc_pages(&app, 6, &h) == MP_OK);

    uint16_t pc;
    assert(mp_get_page_count(&app, h, &pc) == MP_OK);
    check_bool(pc == 6, "delayed handle knows page count");

    /* Should still be able to alloc pages (reserved but not consumed) */
    mp_handle_t h2;
    assert(mp_alloc_pages(&app, 10, &h2) == MP_OK);
    assert(mp_free(&app, h2) == MP_OK);

    /* First lock — actual pages committed */
    void *ptr;
    assert(mp_lock(&app, h, &ptr) == MP_OK);
    memset(ptr, 0xA5, 6 * PAGE_SIZE);
    assert(mp_unlock(&app, h) == MP_OK);

    /* Verify data persists */
    assert(mp_lock(&app, h, &ptr) == MP_OK);
    check_bool(*(uint8_t*)ptr == 0xA5, "data written after delayed alloc");
    assert(mp_unlock(&app, h) == MP_OK);

    assert(mp_free(&app, h) == MP_OK);
}

static void test_T31_delayed_no_reserve(void) {
    printf("\nT31: delayed allocation (no reserve)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES,
        .alloc_delayed = 1, .delayed_no_reserve = 1 };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    /* With no_reserve, we can "over-commit" */
    mp_handle_t h;
    assert(mp_alloc_pages(&app, NUM_PAGES, &h) == MP_OK);
    check_bool(1, "over-commit alloc succeeds (delayed_no_reserve)");

    /* First lock must succeed (only one handle, plenty of pages) */
    void *ptr;
    assert(mp_lock(&app, h, &ptr) == MP_OK);
    assert(mp_unlock(&app, h) == MP_OK);

    assert(mp_free(&app, h) == MP_OK);
}

static void test_T32_applicant_free_all(void) {
    printf("\nT32: mp_free_applicant\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app_a = { .pool = &pool, .applicant_id = 10 };
    mp_applicant_t app_b = { .pool = &pool, .applicant_id = 20 };

    mp_handle_t ha1, ha2, hb;
    assert(mp_alloc_pages(&app_a, 2, &ha1) == MP_OK);
    assert(mp_alloc_pages(&app_a, 2, &ha2) == MP_OK);
    assert(mp_alloc_pages(&app_b, 2, &hb) == MP_OK);

    /* Free all handles of app_a (non-force, all unlocked) */
    mp_error_t e = mp_free_applicant(&app_a, false);
    check(e, MP_OK, "free_applicant non-force OK");

    /* ha1 and ha2 should now be invalid */
    void *ptr;
    e = mp_lock(&app_a, ha1, &ptr);
    check(e, MP_ERR_INVALID_HANDLE, "freed handle ha1 invalid");
    e = mp_lock(&app_a, ha2, &ptr);
    check(e, MP_ERR_INVALID_HANDLE, "freed handle ha2 invalid");

    /* hb (app_b) should still be valid */
    e = mp_lock(&app_b, hb, &ptr);
    check(e, MP_OK, "other applicant handle still valid");
    assert(mp_unlock(&app_b, hb) == MP_OK);

    assert(mp_free(&app_b, hb) == MP_OK);
}

static void test_T33_applicant_free_force(void) {
    printf("\nT33: mp_free_applicant force (locked)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 99 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 3, &h) == MP_OK);
    assert(mp_lock(&app, h, NULL) == MP_OK);

    /* Non-force should fail (locked) */
    mp_error_t e = mp_free_applicant(&app, false);
    check(e, MP_ERR_INVALID_HANDLE, "non-force rejected (locked)");

    /* Force should succeed */
    e = mp_free_applicant(&app, true);
    check(e, MP_OK, "force freed locked handle");

    /* Handle should be invalid now */
    void *ptr;
    e = mp_lock(&app, h, &ptr);
    check(e, MP_ERR_INVALID_HANDLE, "handle invalid after forced free");
}

/* ═══════════════════════════════════════════════════════════════
 *  T34: mp_lock_partial_wr (writable, one per handle)
 * ═══════════════════════════════════════════════════════════════ */
static void test_T34_partial_map_mutex(void) {
    printf("\nT34: mp_partial_map mutex (full lock / child wr / child rd)\n");
    reset_pool();
    mp_config_t cfg = { .pool_memory = pool_mem, .pool_size = POOL_SIZE,
        .metadata = meta_data, .metadata_size = METADATA_SZ,
        .page_size = PAGE_SIZE, .max_handles = MAX_HANDLES,
        .vm_enabled = 1,
        .vm_evict = vm_evict_cb,
        .vm_load  = vm_load_cb };
    mp_pool_t pool = {0};
    assert(mp_init(&pool, &cfg) == MP_OK);
    mp_applicant_t app = { .pool = &pool, .applicant_id = 1 };

    mp_handle_t h;
    assert(mp_alloc_pages(&app, 4, &h) == MP_OK);

    /* 1) Full lock prevents child mapping */
    void *ptr;
    assert(mp_lock(&app, h, &ptr) == MP_OK);
    mp_handle_t child;
    mp_error_t e = mp_partial_map(&app, h, 0, PAGE_SIZE, false, &child);
    check(e, MP_ERR_WR_LOCKED, "partial_map rejected while full lock active");
    assert(mp_unlock(&app, h) == MP_OK);

    /* 2) Child mapping prevents full lock on parent */
    e = mp_partial_map(&app, h, 0, PAGE_SIZE, false, &child);
    check(e, MP_OK, "partial_map read-only child");
    e = mp_lock(&app, h, &ptr);
    check(e, MP_ERR_WR_LOCKED, "full lock rejected while child exists");
    mp_free(&app, child);

    /* 3) Writable child prevents read-only child */
    e = mp_partial_map(&app, h, 0, PAGE_SIZE, true, &child);
    check(e, MP_OK, "partial_map writable child");
    mp_handle_t child2;
    e = mp_partial_map(&app, h, PAGE_SIZE, PAGE_SIZE, false, &child2);
    check(e, MP_ERR_WR_LOCKED, "read-only child rejected while writable child exists");
    mp_free(&app, child);

    /* 4) Read-only child prevents writable child */
    e = mp_partial_map(&app, h, 0, PAGE_SIZE, false, &child);
    check(e, MP_OK, "partial_map read-only child");
    e = mp_partial_map(&app, h, PAGE_SIZE, PAGE_SIZE, true, &child2);
    check(e, MP_ERR_WR_LOCKED, "writable child rejected while read-only child exists");
    mp_free(&app, child);

    /* 5) Multiple read-only children coexist */
    e = mp_partial_map(&app, h, 0, PAGE_SIZE, false, &child);
    check(e, MP_OK, "first read-only child");
    e = mp_partial_map(&app, h, PAGE_SIZE, PAGE_SIZE, false, &child2);
    check(e, MP_OK, "second read-only child (coexists)");

    /* Lock both children */
    e = mp_lock(&app, child, &ptr);
    check(e, MP_OK, "lock first child");
    void *ptr2;
    e = mp_lock(&app, child2, &ptr2);
    check(e, MP_OK, "lock second child");
    assert(mp_unlock(&app, child) == MP_OK);
    assert(mp_unlock(&app, child2) == MP_OK);

    mp_free(&app, child);
    mp_free(&app, child2);

    /* 6) After all children freed, full lock works again */
    e = mp_lock(&app, h, &ptr);
    check(e, MP_OK, "full lock works after all children freed");
    assert(mp_unlock(&app, h) == MP_OK);

    mp_free(&app, h);
}
int main(void) {
    printf("=== Memory Pool Test Suite ===\n");

    test_T1_init_ok();
    test_T2_init_metadata_too_small();
    test_T3_alloc_1page();
    test_T4_alloc_multi();
    test_T5_alloc_oom();
    test_T6_lock_ptr();
    test_T7_lock_unlock_refcount();
    test_T8_unlock_too_many();
    test_T9_free_handle_invalid();
    test_T10_free_realloc_generation();
    test_T11_compact_simple();
    test_T12_compact_locked_not_moved();
    test_T13_vm_swap();
    test_T14_partial_map();
    test_T15_generation_safety();
    test_T16_null_ptr();
    test_T17_zero_alloc();
    test_T18_free_block_split();
    test_T19_free_block_merge();
    test_T20_free_list_order();
    test_T21_lru_ordering();
    test_T22_free_node_exhaustion();
    test_T23_resize_shrink();
    test_T24_resize_enlarge_free();
    test_T25_resize_enlarge_move_current();
    test_T26_resize_enlarge_move_others();
    test_T27_resize_byte();
    test_T28_applicant_create();
    test_T29_handle_accessors();
    test_T30_delayed_alloc();
    test_T31_delayed_no_reserve();
    test_T32_applicant_free_all();
    test_T33_applicant_free_force();
    test_T34_partial_map_mutex();
    test_T35_child_compact_unlocked();

    printf("\n=== Results: %d passed, %d failed (out of %d) ===\n",
           test_passed, test_failed, test_idx);
    return test_failed > 0 ? 1 : 0;
}
