# mp_pool — Developer Guide

**Language: English** · [中文版](DEVELOPER_zh.md)

This document describes the internal architecture, data structures, algorithms, and design decisions behind the mp_pool memory pool system.

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Memory Layout](#memory-layout)
- [Internal Data Structures](#internal-data-structures)
- [Core Algorithms](#core-algorithms)
- [VM Swap Mechanism](#vm-swap-mechanism)
- [Error Handling Strategy](#error-handling-strategy)
- [Testing](#testing)
- [Portability Notes](#portability-notes)

---

## Architecture Overview

```
                          User Code
                             │
         ┌───────────────────┼───────────────────┐
         │    mp_pool_t (user-allocated)          │
         │    ┌─────────────────────────────┐     │
         │    │ pool_memory  ptr ───────────┼──→ Memory array (static)
         │    │ metadata     ptr ───────────┼──→ Metadata array (static)
         │    │ page_size, total_pages ...  │     │
         │    │ vm callbacks                │     │
         │    └─────────────────────────────┘     │
         │           API function layer           │
         └────────────────────────────────────────┘
```

The system is split into three physically separate memory regions, all provided by the user:

1. **Pool memory** — The raw storage for user data, divided into fixed-size pages.
2. **Metadata** — All bookkeeping structures (page descriptors, handle table, mapping tables, free-block nodes, bitmaps).
3. **Pool structure** (`mp_pool_t`) — A small struct holding pointers, sizes, and VM callbacks.

This separation means the pool can back memory-mapped peripherals, external PSRAM, or special memory regions simply by pointing `pool_memory` at the appropriate address.

---

## Memory Layout

The metadata region is laid out as follows (offsets are computed at `mp_init` time and stored in the header):

```
Offset  Content                         Size
──────  ─────────────────────────────── ──────────────────────────
0       mp_meta_header_t                sizeof(mp_meta_header_t)
        (magic, version, config,
         free-list head, LRU head/tail,
         table offsets)
        Page Descriptor table           total_pages × sizeof(mp_page_desc_t)
        Handle Entry table              max_handles × sizeof(mp_handle_entry_t)
        VPN→PPN mapping table           total_pages × sizeof(uint16_t)
        PPN→VPN mapping table           total_pages × sizeof(uint16_t)
        Free-block node array           (max_handles + 1) × sizeof(mp_free_block_node_t)
        Handle bitmap                   ceil(max_handles / 8) bytes
        Free-node bitmap                ceil((max_handles + 1) / 8) bytes
```

The total size can be computed at compile time with the `MP_METADATA_SIZE()` macro.

---

## Internal Data Structures

### Metadata Header (`mp_meta_header_t`)

```c
typedef struct {
    uint32_t magic;             /* MP_MAGIC (0x4D505F00) for validation */
    uint16_t version;           /* Format version (currently 1) */
    uint16_t total_pages;       /* Total physical pages */
    uint16_t max_handles;       /* Maximum number of handles */
    uint16_t page_size;         /* Page size in bytes */
    uint16_t free_list_head;    /* Head index of free-block linked list (0xFFFF = empty) */
    uint16_t lru_head;          /* LRU list head (0xFFFF = empty, VM only) */
    uint16_t lru_tail;          /* LRU list tail (0xFFFF = empty, VM only) */
    /* Offsets into metadata for each table (populated by mp_init) */
    uint16_t off_page_desc;
    uint16_t off_handle_entry;
    uint16_t off_vpn2ppn;
    uint16_t off_ppn2vpn;
    uint16_t off_free_nodes;
    uint16_t off_handle_bm;
    uint16_t off_node_bm;
} mp_meta_header_t;
```

### Page Descriptor (`mp_page_desc_t`)

One per physical page:

```c
typedef struct {
    uint16_t flags;          /* 0=free, 1=allocated, 2=swapped (VM) */
    uint16_t lock_count;     /* Reference count; page can be moved only when 0 */
    uint16_t owner_handle;   /* Handle entry index that owns this page */
} mp_page_desc_t;
```

### Handle Entry (`mp_handle_entry_t`)

One per possible handle:

```c
typedef struct {
    uint16_t generation;    /* Incremented on free; paired with handle->generation */
    uint16_t start_vpn;     /* First virtual page number of this allocation */
    uint16_t page_count;    /* Number of pages */
    uint16_t last_access;   /* Timestamp / access counter (reserved) */
    uint16_t prev;          /* LRU linked list prev (0xFFFF = none) */
    uint16_t next;          /* LRU linked list next (0xFFFF = none) */
    uint8_t  allocated;     /* 0 = free slot, 1 = in use */
    uint8_t  delayed;       /* 1 = lazy allocation (pages not yet assigned) */
    uint8_t  wr_locked;     /* 1 = writable partial lock active */
    uint8_t  rd_locks;      /* count of active read-only partial locks */
} mp_handle_entry_t;
```

### Handle (`mp_handle_t`)

The public handle type returned to the user:

```c
typedef struct {
    uint16_t index;       /* Index into the handle entry table */
    uint16_t generation;  /* Must match handle_entry->generation to be valid */
} mp_handle_t;
```

### Free-Block Node (`mp_free_block_node_t`)

Nodes in the free-block singly-linked list:

```c
typedef struct {
    uint16_t start_ppn;   /* First physical page of this free block */
    uint16_t size;        /* Number of contiguous free pages */
    uint16_t next;        /* Next node index (0xFFFF = end of list) */
} mp_free_block_node_t;
```

The node pool has room for exactly `max_handles + 1` entries. This is because `N` allocated handles can create at most `N+1` free intervals between them.

---

## Core Algorithms

### Allocation (`mp_alloc_pages`)

1. Traverse the free-block linked list from `free_list_head`.
2. Find the **first** block whose `size >= num_pages` (first-fit).
3. If the block is larger than needed, **split** it:
   - The node's `start_ppn` is advanced by `num_pages`, `size` decreased accordingly.
   - The first `num_pages` are allocated.
4. If the block exactly matches, remove the node from the list and recycle it.
5. Allocate a handle entry, set up page descriptors, update VPN→PPN and PPN→VPN tables.
6. If VM mode is enabled and no block was found, trigger **eviction** (see below) and retry.

### Free (`mp_free`)

1. Validate the handle (index + generation check).
2. Clear page descriptors and mapping table entries for all owned pages.
3. Allocate a free-block node and insert it into the free list sorted by `start_ppn`.
4. Call `fb_try_merge()`:
   - Scan the list for a node whose end + 1 == new_start (merge with predecessor).
   - Scan for a node whose start == new_end + 1 (merge with successor).
   - If both neighbours exist, merge all three into one.
5. Increment the handle's generation, mark the slot as free, remove from LRU list (VM).

### Compaction (`mp_compact`)

Uses a two-pass approach:

1. **Clear** the free-block list and recycle all nodes.
2. **Scan** PPN 0..total_pages-1:
   - Free / swapped pages: skip.
   - Locked pages: skip the entire locked block.
   - Unlocked allocated pages: identify the contiguous block belonging to the same owner.
     - If the block is not already at `write_ppn`, `memmove` it there.
     - Update VPN→PPN, PPN→VPN, and page descriptors.
3. **Rebuild** the free-block list from the unused tail of the pool (PPN ≥ write_ppn).

Locked pages are never moved; the write cursor jumps past any locked region.

### Resize (`mp_resize_pages`)

The resize operation handles both shrinking and enlarging:

**Shrink** — Truncates pages from the end of the allocation and returns them to the free-block list with automatic coalescing. O(1) amortised.

**Enlarge** — A multi-strategy algorithm:

1. **Free space after**: Scan the PPN range immediately after the current allocation. If all pages are free, extend in-place by updating page descriptors and mapping tables. The free-block list is adjusted via `fb_shrink_overlap`.

2. **Unlocked handles in the way**: If the expansion window contains pages owned by other unlocked handles:
   - Calculate the **total size** of all handles occupying the window.
   - Compare with the **current handle's size**.
   - If current ≤ total: relocate the current handle to a larger free block elsewhere (via `handle_relocate`).
   - If current > total: relocate each obstructing handle to other free blocks, then extend in-place.
   
3. **Locked pages in the way**: Return `MP_ERR_NO_MEMORY` immediately.

The internal `handle_relocate` function handles the mechanics of moving a handle's data and updating all mapping tables:
- In **no-VM mode**: VPN is updated to match the new PPN (identity mapping).
- In **VM mode**: VPN is preserved (logical key for swap backing store); only PPN changes.

### Free-Block Merging (`fb_try_merge`)

After inserting a new free node into the sorted list, the algorithm checks two neighbours:

- **Predecessor**: if `prev.start_ppn + prev.size == new.start_ppn`, they are adjacent → merge by expanding the predecessor.
- **Successor**: if `new.start_ppn + new.size == next.start_ppn`, they are adjacent → merge by expanding the new node and removing the successor.

If both neighbours are adjacent, all three are merged into the predecessor.

---

## VM Swap Mechanism

### LRU List

The handle entry table contains `prev` / `next` indices forming a **doubly-linked list** of all currently allocated handles, ordered by recency of access:

- **Head**: the most recently accessed handle.
- **Tail**: the least recently accessed handle (prime eviction candidate).

Operations on the LRU list are all O(1):

| Operation | Description |
|-----------|-------------|
| `lru_add_to_head(h)` | Add a new handle to the head |
| `lru_remove(h)` | Remove a handle from anywhere in the list |
| `lru_move_to_head(h)` | Move an existing handle to the head (used by `mp_lock`) |
| `lru_get_tail()` | Return the tail handle index |

### Eviction Flow (`vm_evict_until`)

When `mp_alloc_pages` cannot find a suitable free block and VM mode is enabled:

1. Check the free list again (new blocks may have appeared from prior evictions).
2. If still insufficient, select a victim from the **LRU tail**.
3. Walk backward from the tail toward the head looking for a handle whose **all pages are unlocked**.
4. Call `vm_evict` callback to copy the victim's pages to external storage.
5. Mark all victim pages as "swapped" (flags = 2), clear their PPN in VPN→PPN, mark PPNs free in PPN→VPN.
6. Create free-block nodes from the vacated PPNs and merge them into the free list.
7. Remove the victim from the LRU list.
8. Retry the allocation loop.

### Load Flow (in `mp_lock`)

When `mp_lock` encounters a page with PPN == `MP_PPN_INVALID` (swapped out):

1. Scan the free list for any available PPN.
2. Call `vm_load` to restore the page data from external storage.
3. Update VPN→PPN, PPN→VPN, and page descriptor.
4. Move the handle to the LRU head.

### Child Handle (`mp_partial_map`)

`mp_partial_map` creates a **child handle** that maps a sub-range of a parent handle's allocated
pages (VM mode only). Child handles are light-weight records (no page-I/O at creation time) that:

- Can be locked/unlocked via `mp_lock` / `mp_unlock`.
- Are **never swapped out** (they share the parent's physical pages).
- Have their own lock reference count (`full_locked`).
- For a **writable** child, `mp_unlock` triggers `vm_evict` (write‑back).
- `mp_free(child)` unmaps the child mapping only; parent pages remain allocated.

**Three‑way mutual exclusion** (enforced on the parent handle):
| Active State               | Full `mp_lock` | `mp_partial_map` (rd) | `mp_partial_map` (wr) |
|----------------------------|----------------|-----------------------|-----------------------|
| `full_locked > 0`          | Allowed        | Blocked               | Blocked               |
| `child_refs > 0`           | Blocked        | Allowed (rd)          | Blocked (wr)          |
| `child_wr_refs > 0`        | Blocked        | Blocked               | Blocked               |

- `child_refs` = total active child handles, `child_wr_refs` = writable children subset.
- Multiple read‑only children may coexist (`child_refs` increments, `child_wr_refs` unchanged).
- Only one writable child exists at a time (both `child_refs` and `child_wr_refs` increment).

---

## Error Handling Strategy

### Input Validation

Every public function begins with a validation sequence:

1. Check `pool != NULL` → `MP_ERR_NULL_PTR`.
2. Check `pool->metadata != NULL` → `MP_ERR_NULL_PTR`.
3. Check `mh->magic == MP_MAGIC` → `MP_ERR_INVALID_POOL`.
4. For handle operations, call `validate_handle()` which checks index range, allocation state, and generation match.

### Defensive Design

- Bitmap operations are bounded by `max_handles` or `max_handles + 1`.
- Array accesses use index bounds derived from `mh->max_handles` and `mh->total_pages`.
- The bitmap allocators return `0xFFFF` (an invalid index) on exhaustion rather than overflowing.
- All memory offsets are computed at init time and stored, avoiding repeated size calculations.

---

## Testing

### Test Suite

The test file `test_mp_pool.c` covers 27 test scenarios (T1–T27) with 96 individual assertions:

| # | Scenario | What it verifies |
|---|----------|-----------------|
| T1 | Normal init | Pool structure initialised correctly |
| T2 | Metadata too small | Error detection on undersized metadata |
| T3 | Alloc 1 page | Returns valid handle |
| T4 | Alloc multiple pages | Contiguous allocation works |
| T5 | Out of memory | Returns `MP_ERR_NO_MEMORY` |
| T6 | Lock pointer | Pointer points to correct pool location |
| T7 | Lock/unlock refcount | Double lock + double unlock works |
| T8 | Unlock too many | Returns `MP_ERR_NOT_LOCKED` |
| T9 | Free then use | Old handle returns `MP_ERR_INVALID_HANDLE` |
| T10 | Free + realloc | Generation changes, old handle invalid |
| T11 | Compact simple | Fragmented blocks compact correctly |
| T12 | Compact locked | Locked pages not moved during compaction |
| T13 | VM swap | Eviction + reload preserves data integrity |
| T14 | Child handle (read-only) | `mp_partial_map` create + lock + free child |
| T15 | Generation safety | Stale handle rejected after realloc |
| T16 | Null pointers | All API functions reject NULL pool |
| T17 | Zero-size alloc | Zero-size requests rejected |
| T18 | Free block split | First-fit split leaves remainder usable |
| T19 | Free block merge | Adjacent frees merge into one block |
| T20 | Free list order | List is sorted by start_ppn |
| T21 | LRU ordering | LRU tail is evicted first |
| T22 | Node exhaustion | No crash when node pool runs out |
| T23 | Resize shrink | In-place shrink returns excess pages |
| T24 | Resize enlarge (free after) | Extends into free space |
| T25 | Resize enlarge (move current) | Current handle relocated when smaller |
| T26 | Resize enlarge (move others) | Obstructing handles relocated when smaller |
| T27 | Resize byte wrapper | `mp_resize` wrapper works correctly |
| T28 | Applicant create | IDs increment correctly |
| T29 | Handle accessors | `mp_get_size`, `mp_get_ptr`, `mp_get_page_count` |
| T30 | Delayed allocation | Lazy allocation via `mp_alloc_delayed` |
| T31 | Delayed no-reserve | Over-commit works |
| T32 | Free applicant | Non-force free of all handles |
| T33 | Free applicant force | Force-free of a locked handle |
| T34 | `mp_partial_map` mutex | Full lock / child rd / child wr three‑way exclusion |

### Building and Running

```bash
gcc -std=c99 -Wall -Wextra -pedantic -o test_mp_pool mp_pool.c test_mp_pool.c
./test_mp_pool
```

All 96 tests should pass with 0 failures.

---

## Portability Notes

### Compiler Compatibility

The code targets **C99** and uses only:
- Standard integer types (`<stdint.h>`)
- Boolean type (`<stdbool.h>`)
- `memmove` / `memset` (`<string.h>`)

Tested with:
- GCC (x86_64, ARM)
- clang (x86_64)

### Endianness

The metadata header stores a magic number (`0x4D505F00`) which is endianness-sensitive. Pools created on a little-endian system cannot be read on a big-endian system and vice versa. This is not normally a concern for embedded pools, but is noted for completeness.

### Alignment

The metadata region uses natural alignment for all fields. The `mp_meta_header_t` is arranged to minimise padding on both 32-bit and 16-bit platforms. The pool memory itself has no alignment requirements beyond the user-provided base address; individual allocations are page-aligned.

### Atomicity

No atomic operations or memory barriers are used. The pool is **not thread-safe** by design; it is intended for single-threaded or interrupt-disabled contexts typical in bare-metal embedded systems. If multi-threaded access is required, external locking must be applied.
