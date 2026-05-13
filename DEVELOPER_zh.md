# mp_pool — 开发文档

**语言：中文** · [English Version](DEVELOPER_EN.md)

本文档描述了 mp_pool 内存池系统的内部架构、数据结构、算法和设计决策。

---

## 目录

- [架构概览](#架构概览)
- [内存布局](#内存布局)
- [内部数据结构](#内部数据结构)
- [核心算法](#核心算法)
- [VM 换出机制](#vm-换出机制)
- [错误处理策略](#错误处理策略)
- [测试](#测试)
- [许可](#许可)
- [可移植性说明](#可移植性说明)

---

## 架构概览

```
                         用户代码
                             │
         ┌───────────────────┼───────────────────┐
         │    mp_pool_t（用户分配）               │
         │    ┌─────────────────────────────┐     │
         │    │ pool_memory 指针 ──────────┼──→ 内存数组（静态）
         │    │ metadata    指针 ──────────┼──→ 元数据数组（静态）
         │    │ page_size, total_pages ... │     │
         │    │ VM 回调函数                 │     │
         │    └─────────────────────────────┘     │
         │           API 函数层                    │
         └────────────────────────────────────────┘
```

系统在物理上分为三个独立的内存区域，均由用户提供：

1. **池内存** — 用户数据的原始存储空间，按固定大小的页划分。
2. **元数据** — 所有管理结构（页描述符、句柄表、映射表、空闲块节点、位图）。
3. **池结构体**（`mp_pool_t`）— 一个小型结构体，包含指针、大小和 VM 回调。

这种分离使得池只需将 `pool_memory` 指向相应地址，即可支持内存映射外设、外部 PSRAM 或特殊内存区域。

---

## 内存布局

元数据区域的布局如下（偏移量在 `mp_init` 时计算并存储在头中）：

```
偏移    内容                            大小
──────  ─────────────────────────────── ──────────────────────────
0       mp_meta_header_t                sizeof(mp_meta_header_t)
        （魔数、版本、配置、
         空闲链表头、LRU 头/尾、
         各表偏移量）
        页描述符表                      total_pages × sizeof(mp_page_desc_t)
        句柄条目表                      max_handles × sizeof(mp_handle_entry_t)
        VPN→PPN 映射表                  total_pages × sizeof(uint16_t)
        PPN→VPN 映射表                  total_pages × sizeof(uint16_t)
        空闲块节点数组                  (max_handles + 1) × sizeof(mp_free_block_node_t)
        句柄位图                        ceil(max_handles / 8) 字节
        空闲节点位图                    ceil((max_handles + 1) / 8) 字节
```

总大小可在编译时通过 `MP_METADATA_SIZE()` 宏计算。

---

## 内部数据结构

### 元数据头（`mp_meta_header_t`）

```c
typedef struct {
    uint32_t magic;             /* MP_MAGIC (0x4D505F00)，用于验证 */
    uint16_t version;           /* 格式版本（当前为 1） */
    uint16_t total_pages;       /* 物理页总数 */
    uint16_t max_handles;       /* 最大句柄数 */
    uint16_t page_size;         /* 页大小（字节） */
    uint16_t free_list_head;    /* 空闲块链表头索引（0xFFFF = 空） */
    uint16_t lru_head;          /* LRU 链表头（0xFFFF = 空，仅 VM） */
    uint16_t lru_tail;          /* LRU 链表尾（0xFFFF = 空，仅 VM） */
    /* 各表在元数据中的偏移量（由 mp_init 填充） */
    uint16_t off_page_desc;
    uint16_t off_handle_entry;
    uint16_t off_vpn2ppn;
    uint16_t off_ppn2vpn;
    uint16_t off_free_nodes;
    uint16_t off_handle_bm;
    uint16_t off_node_bm;
} mp_meta_header_t;
```

### 页描述符（`mp_page_desc_t`）

每个物理页一个：

```c
typedef struct {
    uint16_t flags;          /* 0=空闲, 1=已分配, 2=已换出(VM) */
    uint16_t lock_count;     /* 引用计数；仅当为 0 时才可移动 */
    uint16_t owner_handle;   /* 拥有该页的句柄条目索引 */
    uint16_t ro_share_refs;  /* 共享此页的只读子句柄数（>0 时禁止换出） */
} mp_page_desc_t;
```

### 句柄条目（`mp_handle_entry_t`）

每个可能的句柄一个：

```c
typedef struct {
    uint16_t generation;    /* 释放时自增；与 handle->generation 配对验证 */
    uint16_t start_vpn;     /* 该分配的起始虚拟页号 */
    uint16_t page_count;    /* 页数 */
    uint16_t last_access;   /* 时间戳/访问计数器（保留） */
    uint16_t prev;          /* LRU 链表前驱（0xFFFF = 无） */
    uint16_t next;          /* LRU 链表后继（0xFFFF = 无） */
    uint16_t applicant_id;  /* 拥有此句柄的申请者 */
    uint8_t  allocated;       /* 0 = 空闲槽位, 1 = 使用中 */
    uint8_t  delayed;         /* 1 = 延迟分配（页尚未分配） */
    uint8_t  full_locked;     /* mp_lock 引用计数（0 = 无） */
    uint16_t parent_handle;   /* 0xFFFF = 根句柄，否则为父句柄索引 */
    uint16_t page_offset;     /* （子句柄）页内字节偏移 */
    uint16_t parent_pg;       /* （子句柄）子覆盖的首个父页 */
    uint8_t  child_type;      /* （子句柄）0=只读, 1=可写, 2=只写 */
    uint8_t  child_refs;      /* （根句柄）活跃子句柄数 */
    uint8_t  child_wr_refs;   /* （根句柄）可写子句柄数 */
    uint8_t  child_wo_refs;   /* （根句柄）只写子句柄数 */
} mp_handle_entry_t;
```

### 句柄（`mp_handle_t`）

返回给用户的公开句柄类型：

```c
typedef struct {
    uint16_t index;       /* 句柄条目表索引 */
    uint16_t generation;  /* 必须与 handle_entry->generation 匹配才有效 */
} mp_handle_t;
```

### 空闲块节点（`mp_free_block_node_t`）

空闲块单向链表中的节点：

```c
typedef struct {
    uint16_t start_ppn;   /* 该空闲块的起始物理页 */
    uint16_t size;        /* 连续空闲页数 */
    uint16_t next;        /* 下一个节点索引（0xFFFF = 链表尾） */
} mp_free_block_node_t;
```

节点池最多容纳 `max_handles + 1` 个条目。这是因为 `N` 个已分配句柄之间最多产生 `N+1` 个空闲区间。

---

## 核心算法

### 分配（`mp_alloc_pages`）

1. 从 `free_list_head` 开始遍历空闲块链表。
2. 找到**第一个** `size >= num_pages` 的块（首次适应）。
3. 若块大于所需，进行**分裂**：
   - 节点的 `start_ppn` 前进 `num_pages`，`size` 相应减少。
   - 前 `num_pages` 被分配出去。
4. 若块恰好匹配，从链表中移除该节点并回收。
5. 分配句柄条目，设置页描述符，更新 VPN→PPN 和 PPN→VPN 表。
6. 若 VM 模式启用且未找到块，触发**换出**（见下文）并重试。

### 释放（`mp_free`）

1. 验证句柄（index + generation 检查）。
2. 清除所有所属页的页描述符和映射表条目。
3. 分配空闲块节点，按 `start_ppn` 排序插入空闲链表。
4. 调用 `fb_try_merge()`：
   - 扫描链表，查找前驱节点（`prev.start + prev.size == new.start`）→ 向前合并。
   - 扫描链表，查找后继节点（`new.start + new.size == next.start`）→ 向后合并。
   - 若前后均相邻，三者合一。
5. 增加句柄的 generation，标记槽位为空闲，从 LRU 链表移除（VM）。

### 压缩（`mp_compact`）

采用两遍扫描法：

1. **清空**空闲块链表并回收所有节点。
2. **扫描** PPN 0..total_pages-1：
   - 空闲/已换出页：跳过。
   - 锁定页：跳过整个锁定块。
   - 未锁定已分配页：识别属于同一所有者的连续块。
     - 若块不在 `write_ppn` 处，用 `memmove` 移动。
     - 更新 VPN→PPN、PPN→VPN 和页描述符。
3. 从池的未使用尾部（PPN ≥ write_ppn）**重建**空闲块链表。

锁定页永远不会被移动；写游标跳过任何锁定区域。

### 调整大小（`mp_resize_pages`）

调整大小操作同时支持缩小和放大：

**缩小** — 截断分配尾部的页面，将其归还到空闲块链表并自动合并。平摊 O(1)。

**放大** — 多策略算法：

1. **后方有空闲空间**：扫描当前分配后方的 PPN 范围。若全部空闲，则通过更新页描述符和映射表原地扩展。通过 `fb_shrink_overlap` 调整空闲块链表。

2. **未锁定句柄挡路**：若扩展窗口包含由其他未锁定句柄拥有的页面：
   - 计算占用窗口的所有句柄的**总大小**。
   - 与**当前句柄的大小**比较。
   - 若当前 ≤ 总大小：将当前句柄搬迁到其他有足够空间的大空闲块中（通过 `handle_relocate`）。
   - 若当前 > 总大小：将每个挡路句柄搬迁到其他空闲块中，然后原地扩展。

3. **锁定页挡路**：立即返回 `MP_ERR_NO_MEMORY`。

内部函数 `handle_relocate` 负责搬迁句柄的数据并更新所有映射表：
- 在**非 VM 模式**下：VPN 更新为与新 PPN 匹配（恒等映射）。
- 在 **VM 模式**下：VPN 保持不变（交换后备存储的逻辑键），仅 PPN 变化。

### 空闲块合并（`fb_try_merge`）

将新空闲节点插入排序链表后，算法检查两个邻居：

- **前驱**：若 `prev.start_ppn + prev.size == new.start_ppn`，则相邻 → 扩展前驱完成合并。
- **后继**：若 `new.start_ppn + new.size == next.start_ppn`，则相邻 → 扩展新节点并移除后继。

若前后均相邻，三者合并入前驱。

---

## VM 换出机制

### LRU 链表

句柄条目表中的 `prev`/`next` 索引构成一个**双向链表**，包含所有当前已分配的句柄，按最近访问时间排序：

- **头部**：最近访问的句柄。
- **尾部**：最久未访问的句柄（首选换出候选）。

LRU 链表的所有操作均为 O(1)：

| 操作 | 说明 |
|------|------|
| `lru_add_to_head(h)` | 将新句柄添加到头部 |
| `lru_remove(h)` | 从链表中任意位置移除句柄 |
| `lru_move_to_head(h)` | 将句柄移到头部（`mp_lock` 中使用） |
| `lru_get_tail()` | 返回尾部句柄索引 |

### 换出流程（`vm_evict_until`）

当 `mp_alloc_pages` 找不到合适的空闲块且 VM 模式启用时：

1. 再次检查空闲链表（之前的换出可能已产生新块）。
2. 若仍不足，从 **LRU 尾部**选择牺牲者。
3. 从尾部向前遍历，寻找**所有页均未锁定**的句柄。
4. 调用 `vm_evict` 回调将牺牲者的页面拷贝到外部存储。
5. 将所有牺牲者页面标记为"已换出"（flags = 2），清除 VPN→PPN 中的 PPN，在 PPN→VPN 中标记 PPN 为空闲。
6. 从腾出的 PPN 创建空闲块节点，合并到空闲链表。
7. 从 LRU 链表中移除牺牲者。
8. 重试分配循环。

### 换入流程（在 `mp_lock` 中）

当 `mp_lock` 遇到 PPN 为 `MP_PPN_INVALID`（已换出）的页时：

1. 扫描空闲链表寻找可用的 PPN。
2. 调用 `vm_load` 从外部存储恢复页面数据。
3. 更新 VPN→PPN、PPN→VPN 和页描述符。
4. 将句柄移到 LRU 头部。

### 子句柄（`mp_partial_map` / `mp_partial_map_write_only`）

子句柄映射父句柄分配页的子范围。三种类型：

| 类型 | `child_type` | 页所有权 | 写回 | 互斥 |
|------|-------------|---------|------|------|
| 只读（RO） | 0 | 共享父页（`ro_share_refs++`），无拷贝 | 无 | 不与 WR/WO 共存 |
| 可写（WR） | 1 | 独立页面（alloc+copy 全部父数据） | `vm_evict`（VM 模式） | 独占（无其他子句柄） |
| 只写（WO） | 2 | 独立页面（alloc+zero），首尾非用户区从父拷贝 | 解锁时写通 | 独占（无其他子句柄） |

**非 VM 模式**：
- 只读子句柄直接共享父页的 PPN，`ro_share_refs` 递增，VPN→PPN 映射指向父页。
- 可写/只写子句柄分配独立页面。

**VM 模式**：
- 只读/可写均触发 alloc+copy（与旧版行为一致）。

**互斥规则**（在父句柄上实施）：
| 活跃状态 | 完整 `mp_lock` | 只读子句柄 | 可写子句柄 | 只写子句柄 |
|---------|---------------|-----------|-----------|-----------|
| `full_locked > 0` | 允许 | 阻止 | 阻止 | 阻止 |
| `child_refs > 0`（有子句柄） | 阻止 | 允许（RO） | 阻止 | 阻止 |
| `child_wr_refs > 0`（有可写） | 阻止 | 阻止 | 阻止 | 阻止 |
| `child_wo_refs > 0`（有只写） | 阻止 | 阻止 | 阻止 | 阻止 |

- 只读子句柄可共存（多个 RO 共享同一父页范围时共用 `ro_share_refs`）。
- 可写/只写子句柄各自独占；只写与所有类型互斥。
- `mp_free` 对只读子句柄递减 `ro_share_refs` 不释放页；对可写/只写释放页。

---

## 错误处理策略

### 输入验证

每个公开函数都以验证序列开始：

1. 检查 `pool != NULL` → `MP_ERR_NULL_PTR`。
2. 检查 `pool->metadata != NULL` → `MP_ERR_NULL_PTR`。
3. 检查 `mh->magic == MP_MAGIC` → `MP_ERR_INVALID_POOL`。
4. 对于句柄操作，调用 `validate_handle()`，检查索引范围、分配状态和 generation 匹配。

### 防御性设计

- 位图操作受 `max_handles` 或 `max_handles + 1` 限制。
- 数组访问使用基于 `mh->max_handles` 和 `mh->total_pages` 的索引边界。
- 位图分配器在耗尽时返回 `0xFFFF`（无效索引）而非溢出。
- 所有内存偏移量在初始化时计算并存储，避免重复的大小计算。

---

## 测试

### 测试套件

测试文件 `test_mp_pool.c` 覆盖 27 个测试场景（T1–T27），包含 96 个独立断言：

| 编号 | 场景 | 验证内容 |
|------|------|---------|
| T1 | 正常初始化 | 池结构正确初始化 |
| T2 | 元数据过小 | 检测并报错 |
| T3 | 分配 1 页 | 返回有效句柄 |
| T4 | 分配多页 | 连续分配正常工作 |
| T5 | 内存不足 | 返回 `MP_ERR_NO_MEMORY` |
| T6 | 锁定指针 | 指针指向正确的池位置 |
| T7 | 锁定/解锁引用计数 | 双锁 + 双解锁正常工作 |
| T8 | 过多解锁 | 返回 `MP_ERR_NOT_LOCKED` |
| T9 | 释放后使用 | 旧句柄返回 `MP_ERR_INVALID_HANDLE` |
| T10 | 释放后重新分配 | generation 变化，旧句柄无效 |
| T11 | 简单压缩 | 碎片块正确压缩 |
| T12 | 锁定页压缩 | 压缩时不移动锁定页 |
| T13 | VM 换出 | 换出 + 换入保持数据完整性 |
| T14 | 子句柄（只读） | `mp_partial_map` 创建 + 锁定 + 释放子句柄 |
| T15 | Generation 安全 | 重新分配后旧句柄被拒绝 |
| T16 | 空指针 | 所有 API 函数拒绝 NULL 池 |
| T17 | 零大小分配 | 拒绝零大小请求 |
| T18 | 空闲块分裂 | 首次适应分裂后剩余部分仍可用 |
| T19 | 空闲块合并 | 相邻释放合并为一个块 |
| T20 | 空闲链表顺序 | 链表按 start_ppn 排序 |
| T21 | LRU 顺序 | LRU 尾部最先被换出 |
| T22 | 节点耗尽 | 节点池耗尽时不崩溃 |
| T23 | 缩小调整 | 原地缩小归还多余页面 |
| T24 | 放大（后方空闲） | 原地扩展到空闲空间 |
| T25 | 放大（搬迁当前） | 当前句柄更小时被搬迁 |
| T26 | 放大（搬迁他人） | 挡路句柄更小时被搬迁 |
| T27 | 字节包装 | `mp_resize` 包装函数正常工作 |
| T28 | 创建申请者 | ID 正确递增 |
| T29 | 句柄访问器 | `mp_get_size`、`mp_get_ptr`、`mp_get_page_count` |
| T30 | 延迟分配 | 通过 `mp_alloc_delayed` 延迟分配 |
| T31 | 延迟无预留 | 超卖正常工作 |
| T32 | 释放申请者 | 非强制释放所有句柄 |
| T33 | 强制释放申请者 | 强制释放已锁定句柄 |
| T34 | 子句柄互斥 | 完整锁 / 只读 / 可写 三路互斥 |
| T35 | 子句柄压缩 | 未锁定的子句柄不阻碍压缩 |
| T36 | OOM 回调 | 内存不足回调触发 |
| T37 | 重复初始化 | 第二次 `mp_init` 返回 `ALREADY_INIT` |
| T38 | 申请者边界 | ID 范围验证 |
| T39 | 只读页共享 | RO 子句柄直接共享父页（无拷贝） |
| T40 | 只写子句柄 | 分配+清零、首尾保留、解锁写通、互斥 |

### 构建和运行

```bash
gcc -std=c99 -Wall -Wextra -pedantic -o test_mp_pool mp_pool.c test_mp_pool.c
./test_mp_pool
```

所有 203 个测试应通过，0 失败。

## 许可

采用 **MIT 许可证**。详情见 [LICENSE](LICENSE) 文件。

**可商用**：允许在任何项目（包括商业项目）中使用、修改和分发本软件。
**需声明原作者**：在软件或文档的所有副本中必须保留版权声明和许可声明。

---

## 可移植性说明

### 编译器兼容性

代码面向 **C99**，仅使用：
- 标准整数类型（`<stdint.h>`）
- 布尔类型（`<stdbool.h>`）
- `memmove` / `memset`（`<string.h>`）

已在以下编译器上测试：
- GCC（x86_64、ARM）
- clang（x86_64）

### 字节序

元数据头存储了一个魔数（`0x4D505F00`），对字节序敏感。在小端系统上创建的池无法在大端系统上读取，反之亦然。这对嵌入式池通常不是问题，但为完整起见在此说明。

### 对齐

元数据区域的所有字段均使用自然对齐。`mp_meta_header_t` 的排列方式在 32 位和 16 位平台上都最小化了填充。池内存本身除用户提供的基地址外没有对齐要求；各个分配按页对齐。

### 原子性

未使用任何原子操作或内存屏障。该池**非线程安全**，专为裸机嵌入式系统中典型的单线程或关中断上下文设计。若需要多线程访问，必须由外部施加锁保护。
