#ifndef MP_POOL_H
#define MP_POOL_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint16_t, uint32_t, uint8_t */
#include <stdbool.h>  /* bool, C99 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ─────────────────────────────────────────────── */
typedef enum {
    MP_OK = 0,
    MP_ERR_NULL_PTR,
    MP_ERR_INVALID_POOL,
    MP_ERR_INVALID_PARAM,
    MP_ERR_NO_MEMORY,
    MP_ERR_OUT_OF_HANDLES,
    MP_ERR_INVALID_HANDLE,
    MP_ERR_METADATA_TOO_SMALL,
    MP_ERR_NOT_LOCKED,
    MP_ERR_VM_NOT_ENABLED,
    MP_ERR_OUT_OF_RANGE,
    MP_ERR_ALREADY_INIT,
    MP_ERR_NO_VICTIM,
    MP_ERR_FREE_NODE_EXHAUSTED,
    MP_ERR_WR_LOCKED,       /* writable partial lock already active on handle */
    MP_ERR_APPLICANT_ID_INVALID,
} mp_error_t;

/* ── OOM callback types ──────────────────────────────────────── */
/* Each callback is a struct carrying a user-data pointer + function pointer.
 * Two variants: simple (no error code) and info (includes mp_error_t). */
typedef void (*mp_oom_fn)(void *user_data, const char *file, int line);
typedef void (*mp_oom_info_fn)(void *user_data, mp_error_t error,
                                const char *file, int line);

typedef struct {
    void     *user_data;
    mp_oom_fn fn;
} mp_oom_cb_t;

typedef struct {
    void          *user_data;
    mp_oom_info_fn fn;
} mp_oom_info_cb_t;

typedef struct {
    mp_oom_cb_t      on_no_memory;       /* MP_ERR_NO_MEMORY */
    mp_oom_cb_t      on_out_of_handles;  /* MP_ERR_OUT_OF_HANDLES */
    mp_oom_cb_t      on_node_exhausted;  /* MP_ERR_FREE_NODE_EXHAUSTED */
    mp_oom_info_cb_t on_oom;             /* any OOM error (passes error code) */
} mp_oom_callbacks_t;

/* ── Forward declarations ────────────────────────────────────── */
typedef struct mp_pool_t mp_pool_t;

/* ── Applicant structure (replaces raw mp_pool_t* in all APIs) ─ */
typedef struct {
    mp_pool_t *pool;
    uint16_t   applicant_id;   /* 0-127 = system, 128+ = user */
} mp_applicant_t;

/* ── Handle type ─────────────────────────────────────────────── */
typedef struct {
    uint16_t index;
    uint16_t generation;
} mp_handle_t;

/* ── Internal structures (exposed for MP_METADATA_SIZE macro) ── */

/* Page descriptor – one per physical page */
typedef struct {
    uint16_t flags;          /* 0=free, 1=allocated, 2=swapped */
    uint16_t lock_count;
    uint16_t owner_handle;   /* handle entry index owning this page */
    uint16_t ro_share_refs;  /* number of RO children sharing this page */
} mp_page_desc_t;

#define MP_FULL_LOCK_MAX 200u  /* max concurrent full locks per handle */

/* Handle entry – one per possible handle */
typedef struct {
    uint16_t generation;
    uint16_t start_vpn;
    uint16_t page_count;
    uint16_t last_access;
    uint16_t prev;           /* LRU prev index (0xFFFF = none) */
    uint16_t next;           /* LRU next index (0xFFFF = none) */
    uint16_t applicant_id;   /* which applicant owns this handle */
    uint8_t  allocated;      /* 0=free, 1=allocated */
    uint8_t  delayed;        /* 1 = lazy allocation (pages not yet assigned) */
    uint8_t  full_locked;    /* mp_lock reference count (0 = none) */
    uint16_t parent_handle;  /* 0xFFFF = root, else index of parent handle */
    uint16_t page_offset;    /* intra-page byte offset (child handle only) */
    uint16_t parent_pg;      /* (child only) first parent-page this child covers */
    uint8_t  child_type;     /* (child) 0=read-only, 1=writable, 2=write-only */
    uint8_t  child_refs;     /* (root only) number of active child handles */
    uint8_t  child_wr_refs;  /* (root only) number of writable children  */
    uint8_t  child_wo_refs;  /* (root only) number of write-only children */
} mp_handle_entry_t;

/* Free-block linked-list node */
typedef struct {
    uint16_t start_ppn;
    uint16_t size;
    uint16_t next;           /* next node index (0xFFFF = end) */
} mp_free_block_node_t;

/* Metadata header – sits at the start of the metadata region */
#define MP_MAGIC 0x4D505F00u  /* "MP_\0" */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t total_pages;
    uint16_t max_handles;
    uint16_t page_size;
    uint16_t next_applicant_id;  /* auto-incrementing, starts at 128 */
    uint16_t reserved_pages;     /* pages reserved by delayed allocations */
    uint16_t free_list_head;     /* free-block list head (0xFFFF = empty) */
    uint16_t lru_head;           /* LRU list head (0xFFFF = empty) */
    uint16_t lru_tail;           /* LRU list tail (0xFFFF = empty) */
    /* Offsets into metadata (populated by mp_init) */
    uint16_t off_page_desc;
    uint16_t off_handle_entry;
    uint16_t off_vpn2ppn;
    uint16_t off_ppn2vpn;
    uint16_t off_free_nodes;
    uint16_t off_handle_bm;
    uint16_t off_node_bm;
} mp_meta_header_t;

/* Special PPN value meaning "not in memory" (VM only) */
#define MP_PPN_INVALID 0xFFFFu

/* ── Macro: compute required metadata size ───────────────────── */
#define MP_METADATA_SIZE(total_pages_, max_handles_)              \
    ( sizeof(mp_meta_header_t)                                    \
    + (size_t)(total_pages_) * sizeof(mp_page_desc_t)             \
    + (size_t)(max_handles_) * sizeof(mp_handle_entry_t)          \
    + (size_t)(total_pages_) * sizeof(uint16_t)  /* VPN→PPN */   \
    + (size_t)(total_pages_) * sizeof(uint16_t)  /* PPN→VPN */   \
    + ((size_t)(max_handles_) + 1) * sizeof(mp_free_block_node_t) \
    + (((size_t)(max_handles_) + 7) / 8)         /* handle BM */ \
    + (((size_t)(max_handles_) + 1 + 7) / 8)     /* node BM */   \
    )

/* ── Configuration structure ─────────────────────────────────── */
typedef struct {
    void    *pool_memory;
    size_t   pool_size;
    void    *metadata;
    size_t   metadata_size;
    size_t   page_size;
    uint16_t max_handles;
    uint8_t  vm_enabled;
    uint8_t  swap_weight;          /* divisor for swap cost (0=default 2) */
    uint8_t  alloc_delayed;        /* 1 = lazy alloc (pages on first lock) */
    uint8_t  delayed_no_reserve;   /* 1 = don't reserve pages either (alloc_delayed must be 1) */
    mp_oom_callbacks_t oom_callbacks;
    void    *vm_user_data;
    void   (*vm_evict)(void *user_data, uint16_t start_vpn,
                       uint16_t num_pages, void *src, size_t len);
    void   (*vm_load)(void *user_data, uint16_t start_vpn,
                      uint16_t num_pages, void *dst, size_t len);
    void   (*vm_clear)(void *user_data, uint16_t start_vpn,
                       uint16_t num_pages, size_t len);
} mp_config_t;

/* ── Memory pool structure (user-allocated) ──────────────────── */
struct mp_pool_t {
    void      *pool_memory;
    size_t     pool_size;
    void      *metadata;
    size_t     metadata_size;
    size_t     page_size;
    uint16_t   total_pages;
    uint8_t    vm_enabled;
    uint8_t    swap_weight;
    uint8_t    alloc_delayed;
    uint8_t    delayed_no_reserve;
    mp_oom_callbacks_t oom_callbacks;
    void      *vm_user_data;
    void     (*vm_evict)(void *, uint16_t, uint16_t, void *, size_t);
    void     (*vm_load)(void *, uint16_t, uint16_t, void *, size_t);
    void     (*vm_clear)(void *, uint16_t, uint16_t, size_t);
};

/* ── Applicant lifecycle ─────────────────────────────────────── */
mp_error_t mp_applicant_create(mp_pool_t *pool, mp_applicant_t *app);
mp_error_t mp_applicant_create_system(mp_pool_t *pool, mp_applicant_t *app,
                                      uint16_t system_id);

/* ── Public API (_fn suffix + file/line) ─────────────────────── */
mp_error_t mp_init_fn         (mp_pool_t *pool, const mp_config_t *cfg,
                               const char *file, int line);
mp_error_t mp_alloc_fn        (mp_applicant_t *app, size_t size,
                               mp_handle_t *out, const char *file, int line);
mp_error_t mp_alloc_pages_fn  (mp_applicant_t *app, uint16_t num_pages,
                               mp_handle_t *out, const char *file, int line);
mp_error_t mp_lock_fn         (mp_applicant_t *app, mp_handle_t handle,
                               void **out_ptr, const char *file, int line);
mp_error_t mp_unlock_fn       (mp_applicant_t *app, mp_handle_t handle,
                               const char *file, int line);
mp_error_t mp_free_fn         (mp_applicant_t *app, mp_handle_t handle,
                               const char *file, int line);
mp_error_t mp_resize_fn       (mp_applicant_t *app, mp_handle_t *handle,
                               size_t new_size, const char *file, int line);
mp_error_t mp_resize_pages_fn (mp_applicant_t *app, mp_handle_t *handle,
                               uint16_t new_num_pages, const char *file, int line);
mp_error_t mp_free_applicant_fn(mp_applicant_t *app, bool force,
                                const char *file, int line);
mp_error_t mp_compact_fn      (mp_applicant_t *app, const char *file, int line);

/* ── Child handle (partial map) API ─────────────────────────── */
mp_error_t mp_partial_map_fn  (mp_applicant_t *app, mp_handle_t parent,
                               size_t offset, size_t length, bool writable,
                               mp_handle_t *out_child, const char *file, int line);
mp_error_t mp_partial_map_write_only_fn(mp_applicant_t *app, mp_handle_t parent,
                                        size_t offset, size_t length,
                                        mp_handle_t *out_child,
                                        const char *file, int line);

/* ── Handle accessors ────────────────────────────────────────── */
mp_error_t mp_get_size_fn      (mp_applicant_t *app, mp_handle_t handle,
                                size_t *out_size, const char *file, int line);
mp_error_t mp_get_page_count_fn(mp_applicant_t *app, mp_handle_t handle,
                                uint16_t *out_pages, const char *file, int line);
mp_error_t mp_get_ptr_fn       (mp_applicant_t *app, mp_handle_t handle,
                                void **out_ptr, const char *file, int line);

/* ── OOM callback setter ─────────────────────────────────────── */
mp_error_t mp_set_oom_callbacks_fn(mp_applicant_t *app,
                                   const mp_oom_callbacks_t *cb,
                                   const char *file, int line);

/* ── Macro wrappers (add __FILE__ / __LINE__) ─────────────────── */
#ifdef MP_DEBUG
#  define mp_init(pool, cfg)                mp_init_fn(pool, cfg, __FILE__, __LINE__)
#  define mp_alloc(app, sz, out)            mp_alloc_fn(app, sz, out, __FILE__, __LINE__)
#  define mp_alloc_pages(app, np, out)      mp_alloc_pages_fn(app, np, out, __FILE__, __LINE__)
#  define mp_lock(app, h, ptr)              mp_lock_fn(app, h, ptr, __FILE__, __LINE__)
#  define mp_unlock(app, h)                 mp_unlock_fn(app, h, __FILE__, __LINE__)
#  define mp_free(app, h)                   mp_free_fn(app, h, __FILE__, __LINE__)
#  define mp_resize(app, h, sz)             mp_resize_fn(app, h, sz, __FILE__, __LINE__)
#  define mp_resize_pages(app, h, np)       mp_resize_pages_fn(app, h, np, __FILE__, __LINE__)
#  define mp_free_applicant(app, force)     mp_free_applicant_fn(app, force, __FILE__, __LINE__)
#  define mp_compact(app)                   mp_compact_fn(app, __FILE__, __LINE__)
#  define mp_partial_map(app, parent, off, len, wr, child) \
        mp_partial_map_fn(app, parent, off, len, wr, child, __FILE__, __LINE__)
#  define mp_partial_map_write_only(app, parent, off, len, child) \
        mp_partial_map_write_only_fn(app, parent, off, len, child, __FILE__, __LINE__)
#  define mp_get_size(app, h, sz)           mp_get_size_fn(app, h, sz, __FILE__, __LINE__)
#  define mp_get_page_count(app, h, pc)     mp_get_page_count_fn(app, h, pc, __FILE__, __LINE__)
#  define mp_get_ptr(app, h, ptr)           mp_get_ptr_fn(app, h, ptr, __FILE__, __LINE__)
#  define mp_set_oom_callbacks(app, cb)     mp_set_oom_callbacks_fn(app, cb, __FILE__, __LINE__)
#else
#  define mp_init(pool, cfg)                mp_init_fn(pool, cfg, NULL, 0)
#  define mp_alloc(app, sz, out)            mp_alloc_fn(app, sz, out, NULL, 0)
#  define mp_alloc_pages(app, np, out)      mp_alloc_pages_fn(app, np, out, NULL, 0)
#  define mp_lock(app, h, ptr)              mp_lock_fn(app, h, ptr, NULL, 0)
#  define mp_unlock(app, h)                 mp_unlock_fn(app, h, NULL, 0)
#  define mp_free(app, h)                   mp_free_fn(app, h, NULL, 0)
#  define mp_resize(app, h, sz)             mp_resize_fn(app, h, sz, NULL, 0)
#  define mp_resize_pages(app, h, np)       mp_resize_pages_fn(app, h, np, NULL, 0)
#  define mp_free_applicant(app, force)     mp_free_applicant_fn(app, force, NULL, 0)
#  define mp_compact(app)                   mp_compact_fn(app, NULL, 0)
#  define mp_partial_map(app, parent, off, len, wr, child) \
        mp_partial_map_fn(app, parent, off, len, wr, child, NULL, 0)
#  define mp_partial_map_write_only(app, parent, off, len, child) \
        mp_partial_map_write_only_fn(app, parent, off, len, child, NULL, 0)
#  define mp_get_size(app, h, sz)           mp_get_size_fn(app, h, sz, NULL, 0)
#  define mp_get_page_count(app, h, pc)     mp_get_page_count_fn(app, h, pc, NULL, 0)
#  define mp_get_ptr(app, h, ptr)           mp_get_ptr_fn(app, h, ptr, NULL, 0)
#  define mp_set_oom_callbacks(app, cb)     mp_set_oom_callbacks_fn(app, cb, NULL, 0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* MP_POOL_H */
