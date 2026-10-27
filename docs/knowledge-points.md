# 《深入 Rust IndexMap》知识点完整提炼

> 来源：微信公众号文章《深入 Rust IndexMap：一行代码 inserts，为何能保序、快、省？》
> （https://mp.weixin.qq.com/s/FLaHXQQaCPijFGCVy3rsSQ）
> 本文同时作为 `indexmap-cpp` 实现的设计依据。文中"原文"指该文章；"本实现"指本仓库的 C++ 移植。

---

## 1. 问题背景：HashMap 的无序性

- Rust `HashMap`（以及 C++ `std::unordered_map`）的迭代顺序**不确定**：哈希种子随机（Rust 默认 `RandomState`/SipHash）、扩容重排、实现细节变化都会改变顺序。
- 实际痛点：
  1. **回测不可复现**——同一份数据两次跑，信号计算顺序不同，浮点累加误差不同，收益曲线对不上；
  2. **输出不稳定**——序列化 JSON、导出 CSV 的字段/行顺序每次不同，diff 全是噪音；
  3. **测试难写**——断言只能 `contains_key`，不能断言顺序。
- 常见 workaround 及缺陷：
  | 方案 | 代价 |
  |---|---|
  | 遍历前排序 | 每次多付 O(n log n)，忘了排就重新引入问题 |
  | 改用 `BTreeMap`（C++ `std::map`） | 查找 O(log n) 树比较；顺序是**键序**不是插入序 |
  | `HashMap` + `Vec` 手工同步 | 删除时两处要同步，极易出 bug |
- `IndexMap`（Rust indexmap crate）的目标：**保插入序 + O(1) 增删查**，代价可控。

## 2. 核心数据结构：稠密数组 + 稀疏索引

IndexMap 由两个结构组成（原文配图"IndexMap 的内存布局"）：

```
entries : Vec<Bucket>          indices : 哈希表（hashbrown HashTable）
┌───┬────────────────┐         ┌────┬─────┐
│ 0 │ hash│key│value │ ◄────── │ .. │  0  │   ← 槽位存的是 entries 的下标
│ 1 │ hash│key│value │ ◄────── │ .. │  1  │
│ 2 │ hash│key│value │ ◄────── │ .. │  3  │
│ 3 │ hash│key│value │ ◄───┐  │ .. │  2  │
│ … │    （插入序，无空洞）  │   └──│ .. │ …   │
└───┴────────────────┘         └────┴─────┘
```

1. **`entries`**：稠密 `Vec<Bucket>`，`Bucket { hash: u64, key, value }`。
   - 按**插入顺序**排列，**没有空洞**——这是 O(1) 下标访问 `get_index(i)` 的前提。
   - **缓存 hash**：扩容重建索引、删除修正下标、排序重建表时都要用 hash，存下来避免重复计算。
2. **`indices`**：哈希表（Rust 侧是 hashbrown 的 `HashTable<usize>`），**槽位只存 entries 的下标**（原文：每槽 `usize`(8B) + 1 控制字节；本实现用 `uint32_t`，见 §10）。
   - 表里没有 key/value → 同样容量内存占用更小、cache 更友好。
   - hash 冲突的 key 在表里以"下标不同"体现，比较发生在 entries 上。

## 3. 关键操作的实现

### 3.1 查找 get(k) → Option<&V>
1. `hash = hasher(k)`；
2. 在 indices 表中探测：控制字节存 hash **高 7 位 tag**，组（16 槽）一次 SIMD 比较，tag 命中的槽再取出下标去 entries 比较完整 key；
3. 命中返回 value；探测到 EMPTY 组终止。
- **小容量捷径**：`entries` 为空直接返回 None；只有 1 个元素直接 `k == entries[0].key`，连哈希都不用算。
- 与 HashMap 相比多一次内存访问（下标 → entries 跳转），小规模在 cache 内差距小，大规模多一次 cache miss。

### 3.2 插入 insert(k, v)
- key 已存在：**原位替换 value，位置不动**；
- 新 key：`index = entries.len()` → 哈希表插入下标 → `entries.push_back(Bucket{hash,k,v})`。
- **容量同步**：`entries` 的容量与 indices 表容量对齐（Rust：`try_reserve_exact(min(indices.capacity(), MAX) - len)`），避免 Vec 反复扩容搬移。

### 3.3 SIMD 组探测（SwissTable 原理）
- 控制字节 = hash **高 7 位**（0..0x7F）；特殊值 EMPTY（本实现 0xFF）/ DELETED（0x80）不与任何 tag 相同。
- 一组 16 个控制字节，SSE2 `_mm_cmpeq_epi8 + movemask` **一次比较 16 个槽**，tag 命中才进入慢路径比 key。
- 探测序列：`pos = h1 & mask`，之后按三角步长（+16, +32, +48…）跳组，直到组内出现 EMPTY。
- 负载因子上限 7/8（生长阈值在 44%~87.5% 之间波动，扩容 ×2）。
- 非命中路径完全不吃 key 比较——这是"哈希表查找快"的根源。

### 3.4 删除：两个 API，按需选择（设计教训）
Rust 早期只有 `remove`（保序 O(n)），后来才补 `swap_remove`——**API 设计要显式暴露代价**，不要一个含糊的 `remove`。本实现因此**不提供** `remove`，只提供：

- **`swap_remove(k)`：O(1)，不保序**（适合不在乎中间顺序的场景）：
  1. 哈希表删掉 k 的下标；
  2. `entries.swap_remove(index)`：最后一个元素填进 index 空位；
  3. 若真有元素被移动（index ≠ last），用**被移动元素的缓存 hash** 在表里找到它的下标槽（比较的是下标不是 key），改写为新位置。
  - 全程无 key 比较（用下标比较）、无 hash 重算。
- **`shift_remove(k)`：O(n)，保序**：
  1. 哈希表删下标；`entries.erase(begin+index)`（后段整体左移）；
  2. 修正 index 之后所有元素的下标：**启发式**——若移动元素数 > 容量/2，直接全表扫描把 ∈[index, len) 的下标减 1（顺带 cache 友好）；否则逐元素用缓存 hash 定位修改。
- `pop()`：O(1) 删末尾（表删下标 + `entries.pop_back`）。

### 3.5 排序 sort_keys / sort_by
`entries` 直接 `sort_by`（按 key 比较），然后**用缓存 hash 重建整张 indices 表**——不用重新计算任何 hash。排序后继续插入的新元素依然排在末尾。

### 3.6 retain / move_index / swap_indices / shift_insert
- `retain(pred)`：先在 entries 上做带条件的 retain；**有元素被删才重建哈希表**。
- `move_index(i, j)` / `swap_indices(i, j)`：数组搬移 + 受影响下标修正（move 是区间 ±1，swap 是 O(1)）。
- `shift_insert(i, k, v)`：数组中位插入 + 后段下标 +1。

## 4. API 语义（本实现对齐实现）

| API | 复杂度 | 语义 |
|---|---|---|
| `insert(k,v)` | O(1) | 新键追加；旧键原位换 value，返回 {旧值,是否新插入} |
| `insert_full(k,v)` | O(1) | 同上，额外返回插入下标 |
| `find(k)` / `contains` | O(1) | 查找 |
| `find_equiv(q, qhash, qeq)` | O(1) | **异构查找**：用"等价物"（如 `std::string_view` 查 `std::string` 键）不构造完整 key |
| `get_index(i)` / `operator[]` | O(1) | 按插入序下标访问 |
| `get_index_of(k)` | O(1) | 键 → 插入序下标 |
| `first/last/pop` | O(1) | 端点访问/删除 |
| `swap_remove` / `swap_remove_index` | O(1) | 删除，末元素补位（不保序） |
| `shift_remove` / `shift_remove_index` | O(n) | 删除，保序 |
| `shift_insert` | O(n) | 指定位置插入 |
| `move_index` | O(n) | 移动元素位置 |
| `swap_indices` | O(1) | 交换两个元素位置 |
| `sort_keys` / `sort_by` | O(n log n) | 就地排序（插序信息丢失） |
| `retain` | O(n) | 条件保留 |

异构查找的知识点：HashMap/IndexMap 的哈希与相等由 `BuildHasher`/`Eq` 决定，若 `hash(str_view) == hash(String)` 且 `eq(str_view, String)`，就能不构造 key 查找——C++ 对应自定义 hasher/equal 的模板重载。

## 5. 性能特征（原文基准，Rust，FxBuildHasher）

| 操作 | IndexMap vs HashMap | 原因 |
|---|---|---|
| 插入 100k | 慢 30%~50% | 维护两个结构、双写 |
| 插入 1M | 更慢（1M 时索引表+entries 均大，双写代价放大） | 同上 |
| 命中查找 100k | 慢约 30% | 多一次下标跳转（cache 内代价小） |
| 命中查找 1M | 慢约 2 倍 | 1M 时 entries+表超出 L2/L3，跳转 = 真 cache miss |
| 迭代 100k | **快 4 倍** | 稠密数组顺序扫描 vs 哈希表跳过空槽扫全表 |
| 删 90% 后迭代 | **快 15 倍** | HashMap 删除不缩容，仍扫全表；IndexMap entries 缩到 10% |
| swap_remove | ≈ HashMap::remove | 都是 O(1) |
| shift_remove(下标 0) | O(n)，最坏 | 数组左移 + 全表下标修正 |
| 内存 | 多约 25%（100k，40B 键值） | 8B hash/桶 + 下标表 |

结论：**为"确定性顺序 + O(1) 下标访问"付出的查询/插入代价是常数因子，且换来的迭代与删除后遍历是数量级优势**。

## 6. 适用与不适用

**适用：**
1. 回测/仿真要求**可复现**的遍历顺序；
2. **订单簿**类 `OrderId → Order`：挂单按序插入、成交按位置删（`shift_remove` 恰是队列语义）、盘口快照按下标切片；
3. **有界去重队列**（最老先出）：IndexSet + `shift_remove_index(0)`；
4. **名字 → 下标映射**：初始化 `insert_full` 拿到下标，热循环只走 `get_index`（数组随机访问，无哈希）；
5. **保序序列化**（JSON/CSV 导出顺序稳定）。

**不适用：**
1. 大规模、只查不需序的只读表 → 用 HashMap；
2. 频繁按优先级重排（如 LRU：`move_index` O(n) 每次）→ 专门结构（双向链表 + 哈希）；
3. 需要**键有序**（范围查询、最小键）→ BTreeMap / `std::map`。

## 7. 与 Python dict 的对照（原文延伸）

- Python 3.6+ 的 dict 与 IndexMap **同构**：稠密 entries（`PyDictKeyEntry` 数组，插入序）+ 稀疏 indices（存下标）。保插入序正是这个布局带来的。
- 差异在**删除**：Python 删除在 entries 留**洞**（下标不回收、不压缩），O(1) 保序但 `get_index` 不再紧凑；IndexMap 删除后 entries 始终**稠密**，`get_index` O(1) 严格成立，代价是 `swap_remove`(O(1) 换序) / `shift_remove`(O(n) 保序) 的选择。
- 经验法则（Python 界）：**删得少** → dict 的洞方案好；**边删边按下标访问** → 稠密方案好。IndexMap 选稠密。

## 8. 工程经验（原文总结）

1. 保序需求下，`IndexMap` 比排序/BTreeMap/手工双结构**综合成本最低**；
2. 删除 API 分裂（swap/shift）是**把代价显式化**的设计，值得 C++ 库借鉴；
3. `get_index` 的 O(1) 依赖"entries 无空洞"这一**不变量**——所有删除路径都在维护它；
4. 缓存 hash 是多个热路径（扩容、下标修正、排序重建）的共同前提；
5. SIMD 组探测是现代哈希表（SwissTable/hashbrown/absl flat_hash_map）的公共模式，与容器语义正交。

## 9. HashDoS 与哈希器选择（原文提醒）

- Rust 默认 `RandomState`(SipHash 1-3) 带随机种子，防 HashDoS（外部输入构造全碰撞拖慢查找）。
- 内部产生的整数键用 FxHash 等非加密快哈希。
- C++ 对照：`std::hash<int>` 是恒等映射，**无随机化**——键来自外部输入时应自行提供键控哈希（如 splitmix64 with random seed）。本实现的 Hash 模板参数即接入点。

## 10. 本实现（C++）与原文的差异清单

| 点 | Rust indexmap（原文） | 本实现 indexmap-cpp | 理由 |
|---|---|---|---|
| indices 表 | hashbrown `HashTable<usize>`，删除组内左移无 tombstone | 自研 SwissTable，**tombstone + rehash_in_place** | 可独立证明正确；语义等价，摊还复杂度相同 |
| 槽位宽度 | `usize` = 8B | `uint32_t`（上限 4.29e9 条目） | 表内存减半、cache 更友好；量级上限对目标场景足够 |
| 查找捷径 | len 0/1 直接比 key | 同 | 对齐原文 |
| 异构查找 | `Equivalent` trait | `find_equiv<Q,HashQ,EqQ>` 模板重载 | C++ 无 trait，模板参数等价 |
| HashDoS | RandomState 默认 | 不默认引入随机化（保持确定性），文档提示键控哈希接入点 | 保序可复现正是本库目标；安全由使用方按 §9 决定 |
| OOM | `try_reserve` 可选拒绝分配 | `std::bad_alloc` 自然传播 | C++ 惯例 |
| remove 弃用 | 弃用 `remove` 引导二选一 | **不提供** `remove` | 同一设计教训 |
