# mp_pool — Static Memory Pool System

**Language: English** · [中文版](README_zh.md)

<div align="center">

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Language](https://img.shields.io/badge/language-C99-00599C.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![Tests](https://img.shields.io/badge/tests-203%20passed-brightgreen.svg)
![Platform](https://img.shields.io/badge/platform-embedded%20%7C%20bare--metal-lightgrey.svg)

</div>

A lightweight, static memory pool library written in **C99** for embedded systems and bare-metal environments. No OS calls, no dynamic allocation — all metadata and memory are provided by the user as fixed static arrays.

---

## Table of Contents

- [Features](#features)
- [Quick Start](#quick-start)
- [API Reference](#api-reference)
  - [mp_init](#mp_error_t-mp_init)
  - [mp_alloc / mp_alloc_pages](#mp_error_t-mp_alloc)
  - [mp_lock / mp_unlock](#mp_error_t-mp_lock)
  - [mp_partial_map](#mp_error_t-mp_partial_map)
  - [mp_partial_map_write_only](#mp_error_t-mp_partial_map_write_only)
  - [mp_free](#mp_error_t-mp_free)
  - [mp_resize / mp_resize_pages](#mp_error_t-mp_resize)
  - [mp_compact](#mp_error_t-mp_compact)
- [Configuration](#configuration)
- [Error Codes](#error-codes)
- [VM Mode (Optional)](#vm-mode-optional)
- [Integration Guide](#integration-guide)
- [License](#license)

---

## Features

- **Page-based allocation** — Memory is divided into fixed-size pages; allocations are multiples of the page size.
- **Handle-based safety** — Handles use an `index + generation` tag to detect dangling references automatically.
- **Free-block linked list** — Allocation uses a first-fit strategy over a sorted linked list of free blocks, providing O(free-blocks) performance instead of O(total-pages).
- **Free-block coalescing** — Adjacent free blocks are automatically merged on every free operation.
- **Compaction (defragmentation)** — Unlocked allocated pages can be compacted toward PPN 0 to eliminate fragmentation.
- **Reference counting** — Each page maintains a lock count; multiple concurrent locks are supported.
- **Optional VM mode** — Pages can be swapped out to external storage via user callbacks, transparent to upper layers. An LRU doubly-linked list provides O(1) victim selection.
- **Pure C99** — No compiler extensions, no OS calls, no dynamic memory. Suitable for GCC ARM, IAR, Keil, and other MCU toolchains.
- **Fully deterministic** — All metadata structures are pre-allocated; memory usage is known at compile time.

---

## Quick Start

### 1. Define your pool parameters

```c
#include "mp_pool.h"

#define POOL_PAGE_SIZE    256     /* bytes per page (must be power of 2, ≥ 16) */
#define POOL_NUM_PAGES    64      /* total pages in the pool */
#define POOL_MAX_HANDLES  16      /* maximum number of simultaneously alive handles */
#define POOL_SIZE         ((size_t)POOL_NUM_PAGES * POOL_PAGE_SIZE)
#define POOL_META_SZ      MP_METADATA_SIZE(POOL_NUM_PAGES, POOL_MAX_HANDLES)
```

### 2. Declare static arrays

```c
static uint8_t pool_memory[POOL_SIZE];        /* the memory pool itself */
static uint8_t pool_metadata[POOL_META_SZ];   /* bookkeeping data */
static mp_pool_t my_pool;                     /* pool control structure */
```

### 3. Initialise the pool

```c
mp_config_t cfg = {
    .pool_memory   = pool_memory,
    .pool_size     = POOL_SIZE,
    .metadata      = pool_metadata,
    .metadata_size = POOL_META_SZ,
    .page_size     = POOL_PAGE_SIZE,
    .max_handles   = POOL_MAX_HANDLES,
    .vm_enabled    = 0,        /* disable VM mode for now */
};

mp_error_t err = mp_init(&my_pool, &cfg);
if (err != MP_OK) { /* handle error */ }
```

### 4. Allocate and use memory

```c
mp_handle_t handle;
void *ptr;

/* Allocate 3 pages */
err = mp_alloc_pages(&my_pool, 3, &handle);
if (err != MP_OK) { /* handle OOM */ }

/* Lock to get a writable pointer (increments refcount) */
err = mp_lock(&my_pool, handle, &ptr);
if (err != MP_OK) { /* handle error */ }

memset(ptr, 0xAB, 3 * POOL_PAGE_SIZE);   /* use the memory */

/* Unlock when done (decrements refcount) */
mp_unlock(&my_pool, handle);

/* Free the handle when no longer needed */
mp_free(&my_pool, handle);
```

### 5. Allocate by byte size

```c
mp_handle_t h;
mp_alloc(&my_pool, 1000, &h);    /* internally rounds up to 4 pages (1024 bytes) */
```

---

## API Reference

### `mp_error_t mp_init(mp_pool_t *pool, const mp_config_t *cfg)`

Initialises a memory pool.

| Parameter | Description |
|-----------|-------------|
| `pool` | Pointer to a user-allocated `mp_pool_t` |
| `cfg` | Configuration structure (see [Configuration](#configuration)) |

Returns `MP_OK` on success, or an error code.

---

### `mp_error_t mp_alloc(mp_pool_t *pool, size_t size, mp_handle_t *out)`

Allocate memory by byte size.

| Parameter | Description |
|-----------|-------------|
| `size` | Requested size in bytes |
| `out` | Output: receives the handle |

The size is rounded up to the next page boundary. Equivalent to calling `mp_alloc_pages` with `(size + page_size - 1) / page_size`.

---

### `mp_error_t mp_alloc_pages(mp_pool_t *pool, uint16_t num_pages, mp_handle_t *out)`

Allocate memory by page count.

| Parameter | Description |
|-----------|-------------|
| `num_pages` | Number of contiguous pages requested |
| `out` | Output: receives the handle |

Uses a **first-fit** policy over the free-block linked list. If the found block is larger than needed, it is **split** and the remainder stays in the free list.

---

### `mp_error_t mp_lock(mp_pool_t *pool, mp_handle_t handle, void **out_ptr)`

Lock a handle's pages and get a pointer to the memory.

| Parameter | Description |
|-----------|-------------|
| `handle` | The handle to lock |
| `out_ptr` | Output: pointer to the first page of the allocation (can be `NULL`) |

- Increments the lock count on every page owned by the handle.
- In VM mode, if a page has been swapped out, it is transparently loaded back.
- In VM mode, the handle is moved to the head of the LRU list.

---

### `mp_error_t mp_partial_map(mp_pool_t *pool, mp_handle_t parent, size_t offset, size_t length, bool writable, mp_handle_t *out_child)`

Create a **child handle** that maps a sub-range of a parent handle's pages.

| Parameter | Description |
|-----------|-------------|
| `pool` | Memory pool |
| `parent` | The parent handle |
| `offset` | Byte offset from the start of the parent allocation |
| `length` | Number of bytes (must be ≥ 1) |
| `writable` | `true` = writable child (independent copy), `false` = read-only child (shared pages) |
| `out_child` | Output: the newly created child handle |

**Non-VM mode**:
- Read-only children share the parent's pages directly (no data copy). `ro_share_refs` is incremented on each shared page.
- Writable children allocate independent pages and copy all parent data.

**VM mode**:
- Same behaviour as legacy: both read-only and writable trigger alloc+copy.

**Mutual exclusion**:
| Active state | Full `mp_lock` | Read-only child | Writable child |
|-------------|---------------|----------------|---------------|
| `full_locked > 0` | allowed | blocked | blocked |
| `child_refs > 0` (any child exists) | blocked | allowed (RO) | blocked |
| `child_wr_refs > 0` (writable child exists) | blocked | blocked | blocked |

- Read-only children may coexist; writable children are exclusive.
- Child handles can be locked/unlocked with `mp_lock` / `mp_unlock`.
- `mp_free(child)` for read-only children only decrements `ro_share_refs` (does not free pages); for writable children it frees pages.

---

### `mp_error_t mp_partial_map_write_only(mp_pool_t *pool, mp_handle_t parent, size_t offset, size_t length, mp_handle_t *out_child)`

Create a **write-only child handle** that allocates independent pages and automatically writes them back to the parent on unlock.

| Parameter | Description |
|-----------|-------------|
| `pool` | Memory pool |
| `parent` | The parent handle |
| `offset` | Byte offset from the start of the parent allocation |
| `length` | Number of bytes (must be ≥ 1) |
| `out_child` | Output: the newly created child handle |

- Allocates independent pages (all zeroed), **only** copies non-user boundary bytes from parent:
  - First page: bytes before `page_off` are preserved from parent.
  - Last page: bytes after `end_off` are preserved from parent.
  - User range + middle pages: zeroed (no parent read needed).
- **On unlock**: all child pages are automatically written back to the parent (write-through semantics).
- **Mutual exclusion**: exclusive with ALL other child types (read-only, writable, other write-only).
- Freeing releases child pages and decrements `child_wo_refs`.

---

### `mp_error_t mp_unlock(mp_pool_t *pool, mp_handle_t handle)`

Unlock a handle's pages (decrement lock count).

| Parameter | Description |
|-----------|-------------|
| `handle` | The handle to unlock |

- For a **writable child** (`child_type = 1`), after decrementing locks, triggers `vm_evict` (write‑back to external storage).
- For a **write-only child** (`child_type = 2`), after decrementing locks, automatically writes all child pages back to the parent (write-through).
- Returns `MP_ERR_NOT_LOCKED` only if **no** page of this handle was locked at all.

---

### `mp_error_t mp_free(mp_pool_t *pool, mp_handle_t handle)`

Release a handle.

| Parameter | Description |
|-----------|-------------|
| `handle` | The handle to free |

- Increments the handle's generation, invalidating any copies of the old handle value.
- **Read-only children** (shared pages): decrements `ro_share_refs` on each page; does **not** free physical pages. Parent pages remain valid.
- **Writable / write-only children** (independent pages): frees all child pages to the free pool, automatically **merged** with adjacent free blocks.
- **Root handles**: frees all pages, updates parent's `child_refs` / `child_wr_refs` / `child_wo_refs`.
- In VM mode, the handle is removed from the LRU list.

---

### `mp_error_t mp_resize(mp_pool_t *pool, mp_handle_t *handle, size_t new_size)`

Resize a handle's allocation by byte size.

| Parameter | Description |
|-----------|-------------|
| `handle` | Pointer to the handle (may be updated if relocation occurs) |
| `new_size` | Target size in bytes |

The size is rounded up to the next page boundary. See `mp_resize_pages` for details of the resize algorithm.

---

### `mp_error_t mp_resize_pages(mp_pool_t *pool, mp_handle_t *handle, uint16_t new_num_pages)`

Resize a handle's allocation by page count.

| Parameter | Description |
|-----------|-------------|
| `handle` | Pointer to the handle (may be updated if relocation occurs) |
| `new_num_pages` | Target number of pages |

**Shrinking** — Excess pages at the end of the allocation are returned to the free pool and merged with any adjacent free blocks. The handle is updated in-place.

**Enlarging** — The algorithm works as follows:

1. If all pages immediately after the current allocation are **free**, the allocation is extended in-place.
2. If some pages are occupied by **unlocked** handles:
   - The total size of the handles in the way is compared with the current handle's size.
   - If the current handle is **smaller or equal**, it is moved to a new location with enough space.
   - If the handles in the way are **smaller**, they are relocated elsewhere, freeing up the space for in-place extension.
3. If any page in the expansion window is **locked**, the operation fails with `MP_ERR_NO_MEMORY`.

In all cases, handle data is preserved and the handle's identity (`index + generation`) is kept valid.

---

### `mp_error_t mp_compact(mp_pool_t *pool)`

Defragment the pool by moving unlocked allocated pages toward PPN 0.

- Locked pages are never moved.
- The free-block list is rebuilt after compaction.

---

## Configuration

### `mp_config_t`

| Field | Type | Description |
|-------|------|-------------|
| `pool_memory` | `void *` | Start address of the memory pool (user static array) |
| `pool_size` | `size_t` | Total pool size in bytes |
| `metadata` | `void *` | Start address of metadata region (user static array) |
| `metadata_size` | `size_t` | Size of the metadata region (use `MP_METADATA_SIZE`) |
| `page_size` | `size_t` | Page size in bytes (power of 2, ≥ 16) |
| `max_handles` | `uint16_t` | Maximum number of concurrently alive handles |
| `vm_enabled` | `uint8_t` | 0 = no-VM mode, 1 = VM mode |
| `vm_user_data` | `void *` | Opaque pointer passed to VM callbacks (may be NULL) |
| `vm_evict` | callback | Called when a page range is swapped out |
| `vm_load` | callback | Called when a page range is loaded back |
| `swap_weight` | `uint8_t` | Swap cost weight (default 2), used in overlap cost calculation |

### `MP_METADATA_SIZE(total_pages, max_handles)` macro

Computes the minimum byte size required for the metadata region. Use this when declaring your static metadata array:

```c
static uint8_t metadata[MP_METADATA_SIZE(64, 16)];
```

The macro accounts for:
- Metadata header (magic, version, offsets)
- Page descriptor table (`total_pages` entries)
- Handle entry table (`max_handles` entries)
- VPN→PPN mapping table (`total_pages` entries)
- PPN→VPN mapping table (`total_pages` entries)
- Free-block node array (`max_handles + 1` entries)
- Handle bitmap (`ceil(max_handles / 8)` bytes)
- Free-node bitmap (`ceil((max_handles + 1) / 8)` bytes)

---

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `MP_OK` | 0 | Success |
| `MP_ERR_NULL_PTR` | 1 | A required pointer was NULL |
| `MP_ERR_INVALID_POOL` | 2 | Pool not initialised (magic number mismatch) |
| `MP_ERR_INVALID_PARAM` | 3 | Invalid parameter (size = 0, etc.) |
| `MP_ERR_NO_MEMORY` | 4 | Not enough free pages to satisfy the request |
| `MP_ERR_OUT_OF_HANDLES` | 5 | Handle table is full |
| `MP_ERR_INVALID_HANDLE` | 6 | Handle index or generation mismatch (stale handle) |
| `MP_ERR_METADATA_TOO_SMALL` | 7 | Metadata array is too small for the requested configuration |
| `MP_ERR_NOT_LOCKED` | 8 | Attempted to unlock a handle with no locked pages |
| `MP_ERR_VM_NOT_ENABLED` | 9 | VM-specific operation called on a non-VM pool |
| `MP_ERR_OUT_OF_RANGE` | 10 | Offset exceeds the allocation size |
| `MP_ERR_ALREADY_INIT` | 11 | Attempted to re-initialise an already-initialised pool |
| `MP_ERR_NO_VICTIM` | 12 | VM mode: all handles have at least one locked page, none can be evicted |
| `MP_ERR_FREE_NODE_EXHAUSTED` | 13 | Free-block node pool exhausted (fragmentation too high) |
| `MP_ERR_WR_LOCKED` | 14 | A lock conflict occurred (full lock / read-only / writable / write-only child exclusion) |
| `MP_ERR_APPLICANT_ID_INVALID` | 15 | Applicant ID is invalid |

---

## VM Mode (Optional)

VM mode adds transparent swap-out/swap-in on top of the basic pool.

### How it works

1. When `mp_alloc_pages` cannot find a sufficient free block and VM mode is enabled, it automatically selects the **least-recently-used** unlocked handle (LRU list tail) and calls the `vm_evict` callback to copy its pages to external storage.
2. The evicted pages are returned to the free pool.
3. Allocation is retried.
4. When `mp_lock` encounters a page marked as swapped out, it calls `vm_load` to transparently restore it.

### Callback signatures

```c
void (*vm_evict)(void *user_data, uint16_t start_vpn,
                 uint16_t num_pages, void *src, size_t len);
void (*vm_load)(void *user_data, uint16_t start_vpn,
                uint16_t num_pages, void *dst, size_t len);
```

- `user_data`: The `vm_user_data` pointer from `mp_config_t`.
- `start_vpn`: Virtual page number of the first page being transferred.
- `num_pages`: Number of pages.
- `src` / `dst`: Pointer into the pool memory.
- `len`: Total byte count (`num_pages * page_size`).

### Example

```c
/* Backing store */
static uint8_t backing_store[64 * 256];

void my_evict(void *ud, uint16_t vpn, uint16_t n, void *src, size_t len) {
    (void)ud;
    memcpy(&backing_store[vpn * 256], src, len);
}
void my_load(void *ud, uint16_t vpn, uint16_t n, void *dst, size_t len) {
    (void)ud;
    memcpy(dst, &backing_store[vpn * 256], len);
}

mp_config_t cfg = {
    /* ... basic fields ... */
    .vm_enabled  = 1,
    .vm_evict    = my_evict,
    .vm_load     = my_load,
};
```

---

## Integration Guide

### Add to your project

**Option A — Copy source files**

Copy `mp_pool.h` and `mp_pool.c` into your project's source tree and add `mp_pool.c` to your build system.

**Option B — As a C module**

```makefile
# Makefile example
CFLAGS  = -std=c99 -Wall -Wextra -pedantic
OBJS    = main.o mp_pool.o

target: $(OBJS)
    $(CC) $(CFLAGS) -o $@ $^

mp_pool.o: mp_pool.c mp_pool.h
    $(CC) $(CFLAGS) -c -o $@ $<
```

### Compiler requirements

- C99 or later.
- Standard headers: `<stddef.h>`, `<stdint.h>`, `<stdbool.h>`, `<string.h>`.

### Memory footprint

- **Pool memory**: `total_pages × page_size` bytes.
- **Metadata**: `MP_METADATA_SIZE(total_pages, max_handles)` bytes (typically a few hundred bytes for moderate configurations).
- **Pool structure** (`mp_pool_t`): ~48 bytes on a 32-bit platform.

All memory is allocated statically at compile time; there is no runtime heap usage.

---

## License

Licensed under the **MIT License**. See [LICENSE](LICENSE) for details.

**Commercial use** is permitted. **Attribution required** — the copyright notice and permission notice must be included in all copies or substantial portions of the Software.

```
MIT License

Copyright (c) 2026 mp_pool contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the condition that the above copyright notice
and this permission notice shall be included in all copies or substantial
portions of the Software.
```
