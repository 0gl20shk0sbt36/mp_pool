# mp_pool — 静态内存池系统

**语言：中文** · [English Version](README_EN.md)

<div align="center">

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Language](https://img.shields.io/badge/language-C99-00599C.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![Tests](https://img.shields.io/badge/tests-203%20passed-brightgreen.svg)
![Platform](https://img.shields.io/badge/platform-embedded%20%7C%20bare--metal-lightgrey.svg)

</div>

一个轻量级的静态内存池库，采用 **C99 标准**编写，专为嵌入式系统和裸机环境设计。无操作系统调用，无动态分配——所有元数据和内存均由用户以固定静态数组提供。

---

## 目录

- [特性](#特性)
- [快速入门](#快速入门)
- [API 参考](#api-参考)
  - [mp_init](#mp_error_t-mp_init)
  - [mp_alloc / mp_alloc_pages](#mp_error_t-mp_alloc)
  - [mp_lock / mp_unlock](#mp_error_t-mp_lock)
  - [mp_partial_map](#mp_error_t-mp_partial_map)
  - [mp_partial_map_write_only](#mp_error_t-mp_partial_map_write_only)
  - [mp_free](#mp_error_t-mp_free)
  - [mp_resize / mp_resize_pages](#mp_error_t-mp_resize)
  - [mp_compact](#mp_error_t-mp_compact)
- [配置](#配置)
- [错误码](#错误码)
- [VM 模式（可选）](#vm-模式可选)
- [集成指南](#集成指南)
- [许可](#许可)

---

## 特性

- **基于页的分配** — 内存按固定大小的页划分；分配大小是页大小的整数倍。
- **基于句柄的安全性** — 句柄采用 `index + generation` 组合，自动检测悬挂引用。
- **空闲块链表** — 分配使用首次适应（first-fit）策略，在按起始地址排序的空闲块链表上搜索，性能为 O(空闲块数) 而非 O(总页数)。
- **空闲块合并** — 每次释放时自动合并相邻空闲块。
- **碎片整理（压缩）** — 可将未锁定的已分配页向 PPN 0 方向移动，消除碎片。
- **引用计数** — 每页维护锁定计数，支持多次并发锁定。
- **可选 VM 模式** — 通过用户回调将冷页换出到外部存储，对上层透明。LRU 双向链表提供 O(1) 的牺牲者选择。
- **纯 C99** — 无编译器扩展，无操作系统调用，无动态内存。适用于 GCC ARM、IAR、Keil 等单片机工具链。
- **完全确定性** — 所有元数据结构预先分配，内存使用在编译时即可确定。

---

## 快速入门

### 1. 定义池参数

```c
#include "mp_pool.h"

#define POOL_PAGE_SIZE    256     /* 每页字节数（必须为 2 的幂，≥ 16） */
#define POOL_NUM_PAGES    64      /* 池中总页数 */
#define POOL_MAX_HANDLES  16      /* 最大同时活跃句柄数 */
#define POOL_SIZE         ((size_t)POOL_NUM_PAGES * POOL_PAGE_SIZE)
#define POOL_META_SZ      MP_METADATA_SIZE(POOL_NUM_PAGES, POOL_MAX_HANDLES)
```

### 2. 声明静态数组

```c
static uint8_t pool_memory[POOL_SIZE];        /* 内存池本体 */
static uint8_t pool_metadata[POOL_META_SZ];   /* 元数据区域 */
static mp_pool_t my_pool;                     /* 池控制结构体 */
```

### 3. 初始化池

```c
mp_config_t cfg = {
    .pool_memory   = pool_memory,
    .pool_size     = POOL_SIZE,
    .metadata      = pool_metadata,
    .metadata_size = POOL_META_SZ,
    .page_size     = POOL_PAGE_SIZE,
    .max_handles   = POOL_MAX_HANDLES,
    .vm_enabled    = 0,        /* 暂时关闭 VM 模式 */
};

mp_error_t err = mp_init(&my_pool, &cfg);
if (err != MP_OK) { /* 处理错误 */ }
```

### 4. 分配和使用内存

```c
mp_handle_t handle;
void *ptr;

/* 分配 3 页 */
err = mp_alloc_pages(&my_pool, 3, &handle);
if (err != MP_OK) { /* 处理内存不足 */ }

/* 锁定以获取可写指针（增加引用计数） */
err = mp_lock(&my_pool, handle, &ptr);
if (err != MP_OK) { /* 处理错误 */ }

memset(ptr, 0xAB, 3 * POOL_PAGE_SIZE);   /* 使用内存 */

/* 使用完毕后解锁（减少引用计数） */
mp_unlock(&my_pool, handle);

/* 不再需要时释放句柄 */
mp_free(&my_pool, handle);
```

### 5. 按字节大小分配

```c
mp_handle_t h;
mp_alloc(&my_pool, 1000, &h);    /* 内部向上取整到 4 页（1024 字节） */
```

---

## API 参考

### `mp_error_t mp_init(mp_pool_t *pool, const mp_config_t *cfg)`

初始化内存池。

| 参数 | 说明 |
|------|------|
| `pool` | 指向用户分配的 `mp_pool_t` 结构体 |
| `cfg` | 配置结构体（见[配置](#配置)） |

成功返回 `MP_OK`，失败返回对应错误码。

---

### `mp_error_t mp_alloc(mp_pool_t *pool, size_t size, mp_handle_t *out)`

按字节大小分配内存。

| 参数 | 说明 |
|------|------|
| `size` | 请求的字节数 |
| `out` | 输出：接收句柄 |

大小向上取整到页边界。等价于以 `(size + page_size - 1) / page_size` 调用 `mp_alloc_pages`。

---

### `mp_error_t mp_alloc_pages(mp_pool_t *pool, uint16_t num_pages, mp_handle_t *out)`

按页数分配内存。

| 参数 | 说明 |
|------|------|
| `num_pages` | 请求的连续页数 |
| `out` | 输出：接收句柄 |

在空闲块链表上采用**首次适应（first-fit）**策略。若找到的块大于请求，则进行**分裂**，剩余部分留在空闲链表中。

---

### `mp_error_t mp_lock(mp_pool_t *pool, mp_handle_t handle, void **out_ptr)`

锁定句柄的页并获取内存指针。

| 参数 | 说明 |
|------|------|
| `handle` | 要锁定的句柄 |
| `out_ptr` | 输出：指向分配区域首地址的指针（可为 NULL） |

- 递增该句柄所拥有的每一页的锁定计数。
- 在 VM 模式下，若某页已被换出，则自动换入。
- 在 VM 模式下，该句柄被移动到 LRU 链表头部。

---

### `mp_error_t mp_partial_map(mp_pool_t *pool, mp_handle_t parent, size_t offset, size_t length, bool writable, mp_handle_t *out_child)`

创建一个**子句柄**，映射父句柄部分页的子范围。

| 参数 | 说明 |
|------|------|
| `pool` | 内存池 |
| `parent` | 父句柄 |
| `offset` | 相对于父分配起始地址的字节偏移 |
| `length` | 字节数（必须 ≥ 1） |
| `writable` | `true` = 可写子句柄（独立拷贝），`false` = 只读子句柄（共享页） |
| `out_child` | 输出：新创建的子句柄 |

**非 VM 模式**：
- 只读子句柄直接共享父页（不拷贝数据），`ro_share_refs` 递增。
- 可写子句柄分配独立页面，拷贝全部父数据。

**VM 模式**：
- 行为同旧版：只读/可写均触发 alloc+copy。

**互斥规则**：
| 活跃状态 | 完整 `mp_lock` | 只读子句柄 | 可写子句柄 |
|----------|---------------|-----------|-----------|
| `full_locked > 0` | 允许 | 阻止 | 阻止 |
| `child_refs > 0`（有子句柄） | 阻止 | 允许（只读） | 阻止 |
| `child_wr_refs > 0`（有可写子句柄） | 阻止 | 阻止 | 阻止 |

- 只读子句柄可共存；可写子句柄独占。
- 子句柄可通过 `mp_lock` / `mp_unlock` 加锁/解锁。
- `mp_free(child)` 对只读子句柄仅递减 `ro_share_refs`（不释放页）；对可写子句柄释放页。

---

### `mp_error_t mp_partial_map_write_only(mp_pool_t *pool, mp_handle_t parent, size_t offset, size_t length, mp_handle_t *out_child)`

创建一个**只写（write-only）子句柄**，分配独立页面，解锁时自动写回父页。

| 参数 | 说明 |
|------|------|
| `pool` | 内存池 |
| `parent` | 父句柄 |
| `offset` | 相对于父分配起始地址的字节偏移 |
| `length` | 字节数（必须 ≥ 1） |
| `out_child` | 输出：新创建的子句柄 |

- 分配独立页面（全部清零），**仅**从父页拷贝首尾页的非用户区域。
  - 首页：偏移前的字节从父页保留。
  - 末页：结束后的字节从父页保留。
  - 用户范围 + 中间页：全零（无需从父读取）。
- **解锁时**：所有子页自动写回父页（写通语义）。
- **互斥**：与所有其他子句柄互斥（只读、可写、其他只写）。
- 释放时释放子页，递减 `child_wo_refs`。

---

### `mp_error_t mp_unlock(mp_pool_t *pool, mp_handle_t handle)`

解锁句柄的页（减少锁定计数）。

| 参数 | 说明 |
|------|------|
| `handle` | 要解锁的句柄 |

- 对于**可写子句柄**（`child_type = 1`），递减锁定后触发 `vm_evict`（写回外部存储）。
- 对于**只写子句柄**（`child_type = 2`），递减锁定后自动将全部子页写回父页（写通）。
- 仅当**没有任何**页被锁定时才返回 `MP_ERR_NOT_LOCKED`。

---

### `mp_error_t mp_free(mp_pool_t *pool, mp_handle_t handle)`

释放句柄。

| 参数 | 说明 |
|------|------|
| `handle` | 要释放的句柄 |

- 递增句柄的 generation，使所有旧句柄值失效。
- **只读子句柄**（共享页）：递减各页的 `ro_share_refs`，**不释放**物理页。父页继续有效。
- **可写/只写子句柄**（独立页）：释放所有子页到空闲池，自动与相邻空闲块**合并**。
- **根句柄**：释放所有页，更新父句柄的 `child_refs` / `child_wr_refs` / `child_wo_refs`。
- 在 VM 模式下，将句柄从 LRU 链表中移除。

---

### `mp_error_t mp_resize(mp_pool_t *pool, mp_handle_t *handle, size_t new_size)`

按字节大小调整句柄的分配空间。

| 参数 | 说明 |
|------|------|
| `handle` | 指向句柄的指针（若发生搬迁可能会更新） |
| `new_size` | 目标大小（字节） |

大小向上取整到页边界。详细算法见 `mp_resize_pages`。

---

### `mp_error_t mp_resize_pages(mp_pool_t *pool, mp_handle_t *handle, uint16_t new_num_pages)`

按页数调整句柄的分配空间。

| 参数 | 说明 |
|------|------|
| `handle` | 指向句柄的指针（若发生搬迁可能会更新） |
| `new_num_pages` | 目标页数 |

**缩小** — 将分配尾部的多余页面归还到空闲池，并与相邻空闲块自动合并。句柄原地更新。

**放大** — 算法流程如下：

1. 若当前分配后方的所有页面均为**空闲**，则原地扩展。
2. 若部分页面被**未锁定**的句柄占用：
   - 比较挡路句柄的总大小与当前句柄的大小。
   - 若当前句柄**更小或相等**，则将其搬迁到有足够空间的新位置。
   - 若挡路句柄**更小**，则将其搬迁到别处，腾出空间供原地扩展。
3. 若扩展窗口中有任何页面被**锁定**，操作失败并返回 `MP_ERR_NO_MEMORY`。

所有情况下，句柄数据均得到保留，句柄标识（`index + generation`）保持有效。

---

### `mp_error_t mp_compact(mp_pool_t *pool)`

通过将未锁定的已分配页向 PPN 0 方向移动来整理碎片。

- 锁定页永远不会被移动。
- 压缩后重建空闲块链表。

---

## 配置

### `mp_config_t`

| 字段 | 类型 | 说明 |
|------|------|------|
| `pool_memory` | `void *` | 内存池起始地址（用户静态数组） |
| `pool_size` | `size_t` | 池总大小（字节） |
| `metadata` | `void *` | 元数据区域起始地址（用户静态数组） |
| `metadata_size` | `size_t` | 元数据区域大小（使用 `MP_METADATA_SIZE`） |
| `page_size` | `size_t` | 页大小（字节，2 的幂，≥ 16） |
| `max_handles` | `uint16_t` | 最大同时活跃句柄数 |
| `vm_enabled` | `uint8_t` | 0 = 非 VM 模式，1 = VM 模式 |
| `vm_user_data` | `void *` | 传递给 VM 回调的不透明指针（可为 NULL） |
| `vm_evict` | 回调 | 页面范围被换出时调用 |
| `vm_load` | 回调 | 页面范围被换入时调用 |
| `swap_weight` | `uint8_t` | 换出代价权重（默认 2），用于重叠检测中计算换出代价 |

### `MP_METADATA_SIZE(total_pages, max_handles)` 宏

计算元数据区域所需的最小字节数。声明静态元数据数组时使用：

```c
static uint8_t metadata[MP_METADATA_SIZE(64, 16)];
```

该宏包含以下部分：
- 元数据头（魔数、版本、偏移量）
- 页描述符表（`total_pages` 条目）
- 句柄条目表（`max_handles` 条目）
- VPN→PPN 映射表（`total_pages` 条目）
- PPN→VPN 映射表（`total_pages` 条目）
- 空闲块节点数组（`max_handles + 1` 条目）
- 句柄位图（`ceil(max_handles / 8)` 字节）
- 空闲节点位图（`ceil((max_handles + 1) / 8)` 字节）

---

## 错误码

| 码 | 值 | 含义 |
|----|----|------|
| `MP_OK` | 0 | 成功 |
| `MP_ERR_NULL_PTR` | 1 | 必要的指针为 NULL |
| `MP_ERR_INVALID_POOL` | 2 | 池未初始化（魔数不匹配） |
| `MP_ERR_INVALID_PARAM` | 3 | 参数无效（size = 0 等） |
| `MP_ERR_NO_MEMORY` | 4 | 空闲页不足，无法满足请求 |
| `MP_ERR_OUT_OF_HANDLES` | 5 | 句柄表已满 |
| `MP_ERR_INVALID_HANDLE` | 6 | 句柄索引或 generation 不匹配（已过期） |
| `MP_ERR_METADATA_TOO_SMALL` | 7 | 元数据数组太小，无法支持请求的配置 |
| `MP_ERR_NOT_LOCKED` | 8 | 尝试解锁一个没有锁定页的句柄 |
| `MP_ERR_VM_NOT_ENABLED` | 9 | 对非 VM 池调用了 VM 专属操作 |
| `MP_ERR_OUT_OF_RANGE` | 10 | 偏移超出分配大小 |
| `MP_ERR_ALREADY_INIT` | 11 | 尝试重复初始化已初始化的池 |
| `MP_ERR_NO_VICTIM` | 12 | VM 模式：所有句柄至少有一页被锁定，无法换出 |
| `MP_ERR_FREE_NODE_EXHAUSTED` | 13 | 空闲块节点池耗尽（碎片化程度过高） |
| `MP_ERR_WR_LOCKED` | 14 | 锁冲突（完整锁/只读/可写/只写子句柄互斥） |
| `MP_ERR_APPLICANT_ID_INVALID` | 15 | 申请者 ID 无效 |

---

## VM 模式（可选）

VM 模式在基本池的基础上增加了透明的换出/换入能力。

### 工作原理

1. 当 `mp_alloc_pages` 找不到足够的空闲块且 VM 模式启用时，自动选择**最久未使用**的未锁定句柄（LRU 链表尾部），调用 `vm_evict` 回调将其页面拷贝到外部存储。
2. 被换出的页面归还到空闲池。
3. 重新尝试分配。
4. 当 `mp_lock` 遇到标记为已换出的页面时，调用 `vm_load` 回调透明地恢复该页。

### 回调签名

```c
void (*vm_evict)(void *user_data, uint16_t start_vpn,
                 uint16_t num_pages, void *src, size_t len);
void (*vm_load)(void *user_data, uint16_t start_vpn,
                uint16_t num_pages, void *dst, size_t len);
```

- `user_data`：`mp_config_t` 中的 `vm_user_data` 指针。
- `start_vpn`：正在传输的第一页的虚拟页号。
- `num_pages`：页数。
- `src` / `dst`：指向池内存的指针。
- `len`：总字节数（`num_pages * page_size`）。

### 示例

```c
/* 后备存储 */
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
    /* ... 基本字段 ... */
    .vm_enabled  = 1,
    .vm_evict    = my_evict,
    .vm_load     = my_load,
};
```

---

## 集成指南

### 添加到项目

**方式 A — 拷贝源文件**

将 `mp_pool.h` 和 `mp_pool.c` 拷贝到项目的源码目录中，并将 `mp_pool.c` 添加到构建系统。

**方式 B — 作为 C 模块**

```makefile
# Makefile 示例
CFLAGS  = -std=c99 -Wall -Wextra -pedantic
OBJS    = main.o mp_pool.o

target: $(OBJS)
    $(CC) $(CFLAGS) -o $@ $^

mp_pool.o: mp_pool.c mp_pool.h
    $(CC) $(CFLAGS) -c -o $@ $<
```

### 编译器要求

- C99 或更高版本。
- 标准头文件：`<stddef.h>`、`<stdint.h>`、`<stdbool.h>`、`<string.h>`。

### 内存占用

- **池内存**：`total_pages × page_size` 字节。
- **元数据**：`MP_METADATA_SIZE(total_pages, max_handles)` 字节（对于中等配置，通常只需几百字节）。
- **池结构体**（`mp_pool_t`）：在 32 位平台上约 48 字节。

所有内存在编译时静态分配，无运行时堆使用。

---

## 许可

采用 **MIT 许可证**。详情见 [LICENSE](LICENSE) 文件。

**可商用**：允许在任何项目（包括商业项目）中使用、修改和分发本软件。
**需声明原作者**：在软件或文档的所有副本中必须保留版权声明和许可声明。

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
