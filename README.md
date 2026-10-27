# indexmap-cpp — 高性能保插入序哈希映射（C++17，头文件库）

版本：**1.0.0**（版本宏 `INDEXMAP_VERSION` 见 `src/index_map.hpp`）

Rust [indexmap](https://docs.rs/indexmap) 的 C++ 移植：**保插入序 + O(1) 增删查 + O(1) 下标访问**。
设计依据来自《深入 Rust IndexMap》一文的知识点提炼（[docs/knowledge-points.md](docs/knowledge-points.md)）。

```
entries : 稠密数组（插入序、无空洞）          indices : SwissTable（槽位存 entries 下标）
┌───┬────────────────┐                       ┌────┬─────┐
│ 0 │ hash│key│value │ ◄──────────────────── │ .. │  0  │
│ 1 │ hash│key│value │ ◄──────────────────── │ .. │  1  │
│ 2 │ hash│key│value │ ◄──────────────────── │ .. │  3  │
│ 3 │ hash│key│value │ ◄───────────────────┐ │ .. │  2  │
│ … │                                    └─→ │ .. │ …   │
└───┴────────────────┘                       └────┴─────┘
```

## 为什么需要它

`std::unordered_map` 迭代顺序不确定：回测不可复现、序列化输出不稳定、测试难写。
常见替代各有代价：遍历前排序多付 O(n log n)；`std::map` 是键序且 O(log n)；手工双结构易出 bug。

## 特性

- **保插入序迭代**：`for (auto [k, v] : m)` 顺序恒等于插入顺序
- **O(1) 下标访问**：`value_at(i)` / `key_at(i)` / `operator[]`（entries 无空洞不变量）
- **O(1) 反查下标**：`get_index_of(k)`
- **自研 SwissTable 索引表**：控制字节（hash 高 7 位）+ SSE2 组探测（一次比 16 槽）+ 三角探测 + 7/8 负载；删除用墓碑 + 原位重排/倍增
- **缓存 hash**：扩容迁移、删除修正下标、排序重建表都不重算哈希
- **删除 API 显式二分**：`swap_remove`（O(1)，末元素补位）/ `shift_remove`（O(n)，保序）——不提供含糊的 `remove`
- **异构查找**：`find_equiv<Q,HashQ,EqQ>`（如 `string_view` 查 `std::string` 键，不构造 key）
- 单头文件、无第三方依赖、C++17；非 x86 自动回退标量路径

## 快速上手

```cpp
#include "src/index_map.hpp"
im::IndexMap<std::string, int> m;
auto [idx, inserted] = m.insert_full("订单A", 100);   // idx == 0
m.insert_full("订单B", 200);                           // idx == 1
m.value_at(0) = 150;                                   // 按下标 O(1) 改值
m.swap_remove("订单A");                                // O(1) 删除（"订单B" 补位到 0）
for (auto [k, v] : m) use(k, v);                       // 恒为插入序
```

## API 与复杂度

| API | 复杂度 | 语义 |
|---|---|---|
| `insert` / `insert_full` | O(1) | 新键追加；旧键原位替换 value、位置不动 |
| `find` / `contains` / `find_equiv` | O(1) | 查找（含空表/单元素捷径：len==1 不算哈希） |
| `get_index_of` | O(1) | 键 → 插入序下标 |
| `key_at` / `value_at` / `operator[]` | O(1) | 下标 → 键/值 |
| `first` / `last` / `pop` | O(1) | 端点访问 / 弹出末尾 |
| `swap_remove[_index]` | O(1) | 删除，末元素补位（**不保序**） |
| `shift_remove[_index]` | O(n) | 删除，保序（启发式：位移多则全表扫描，少则逐元素修正） |
| `shift_insert` | O(n) | 指定位置插入 |
| `move_index` / `swap_indices` | O(n) / O(1) | 调整元素位置 |
| `sort_keys` / `sort_by` | O(n log n) | 就地排序（缓存 hash 重建索引表） |
| `retain` | O(n) | 条件保留（仅确有删除时重建表） |
| `reserve` / `clear` | — | 预留（entries 容量与索引表对齐）/ 清空保留内存 |

## 构建与测试

```bash
make -j$(nproc) test      # 单元测试（-O2）：15 个用例，5.6 万断言
make test_asan            # AddressSanitizer + UBSan 版（手写内存表必跑）
make -j$(nproc) bench     # 性能对比 → output/benchmark-result.txt
```

## 基准结果（g++ 15.2 -O2 -march=native，中位数）

对照：`std::unordered_map`（节点式）、`std::map`；三容器统一 splitmix64 混合哈希（u64→u64）。
完整输出见 [output/benchmark-result.txt](output/benchmark-result.txt)。

| 场景 | N | IndexMap | unordered_map | 比率 | std::map | 比率 |
|---|--:|--:|--:|--:|--:|--:|
| 插入 | 100k | 1.63ms | 3.94ms | **快 2.4x** | 7.38ms | 快 4.5x |
| 插入 | 1M | 47.6ms | 81.2ms | **快 1.7x** | 92.9ms | 快 2.0x |
| 命中查找 | 100k | 0.46ms | 1.19ms | 快 2.6x | 13.5ms | 快 29.6x |
| 命中查找 | 1M | 20.3ms | 38.3ms | 快 1.9x | 588ms | 快 28.9x |
| 未命中查找 | 100k | 0.03ms | 0.22ms | **快 8.0x** | — | — |
| 未命中查找 | 1M | 0.49ms | 4.48ms | **快 9.2x** | — | — |
| 迭代 | 1M | 0.37ms | 14.26ms | **快 38x** | 4.71ms | 快 12.7x |
| 删 90% 后迭代 | 100k | 0.00ms | 0.04ms | **快 15.2x** | 0.03ms | 快 11.4x |
| 删 90% 后迭代 | 1M | 0.03ms | 1.87ms | **快 61x** | 1.28ms | 快 41.7x |
| swap_remove/erase | 1M | 61.0ms | 204.9ms | **快 3.4x** | — | — |
| shift_remove(0) | 100k | ~260ms/千次 | 0.02ms | 慢（保序 O(n) 的代价） | — | — |
| 内存 | 100k | 34 B/元素 | 48 B/元素 | 省 29% | — | — |
| 内存 | 1M | 54 B/元素 | 48 B/元素 | 多 13% | — | — |

注：1M 档同机重复运行波动可达 ±40%（umap 亦然），上表为单轮中位数；结论以 100k 档
（稳定复现）与跨档方向为准。

### 已实施的低延迟优化（cpp-low-latency 清单评审）

| 优化 | 机理 | 实测效果（同轮 IM/umap 比值，多轮验证） |
|---|---|---|
| 探测组单次加载（`__m128i` 两用） | `find_slot` 原每组 2 次 loadu 同地址 → 1 次 | 未命中查找 **-50%**（0.27→0.13，两轮稳定复现；该路径恰省第二次组加载） |
| ctrl+slots 单块内存分配 | 一次 malloc/释放、TLB 与预取友好 | swap_remove 约 -6%（两轮方向一致）；shift_remove(0) -14% |
| `ctrl_is_full`（c < 0x80 单比较） | 替代两次字节比较（grow 迁移/重排/区间扫描） | 摊入慢路径 |
| `__builtin_expect` 分支提示 | 首组即终止占绝大多数（负载 ≤7/8） | 摊入上述收益 |

注：绝对值前后对比会被环境频率漂移污染（同机多轮 umap 自身波动 ±40%），上表结论
采用**同轮 IM/umap 比值**（两容器共享环境，指标环境无关）。命中查找与插入的比值持平
——单次加载对命中路径无净收益（命中只需一次 tag 匹配，第二次组加载本就未执行）。

**与原文（Rust）结论的对照与差异说明**（诚实呈现；详见 docs/knowledge-points.md §10）：

- 原文 IndexMap 插入比 HashMap 慢 30-50%、命中查找慢 30%（1M 慢 2x）——因为 Rust HashMap 是
  hashbrown（开放寻址、无节点分配），对照组本身极快；本基准的 `std::unordered_map` 是**节点式**
  （每元素一次 malloc、迭代靠指针追逐），所以 IndexMap 在插入/删除/迭代上全面占优。
- **方向一致的结论**：1M 命中查找 IndexMap 相对开销增大（下标跳转变真 cache miss）；删 90% 后
  迭代 100k 恰好快 **15.2x**（原文 15x）；内存 1M 时多 ~13%（原文 25%，entries 容量对齐表容量所致）。
- `shift_remove(0)` 的 O(n) 代价如实放大展示（文章同）：需保序高频删除的场景请评估 `swap_remove`。
- 基准方法论注意：`std::hash<u64>` 恒等映射 + 键 < 2^57 会让 SwissTable 的 tag（hash 高 7 位）全为
  0、tag 过滤失效——整数键必须先位混淆（本基准统一 splitmix64；见 docs/knowledge-points.md §9）。

## 适用 / 不适用

**适用**：可复现回测遍历、订单簿（挂单按序、成交按位删）、有界去重队列、名字→下标映射（初始化
`insert_full` 拿下标，热路径纯数组访问）、保序序列化。

**不适用**：大规模只查不需序（用 `unordered_map`/flat_hash_map）；频繁重排（LRU 用双向链表+哈希）；
需要键有序范围查询（用 `std::map`）。

## 已知边界

- 条目数上限 2^32-2（索引表槽位 `uint32_t`；4.29e9 条目以内）
- 键存入后不可修改（迭代代理的 `key` 为 const；改 key 会破坏索引表）
- 非线程安全（同 `std::vector`：并发读安全，读写需外部同步）
- HashDoS：本库不默认随机化哈希（保序可复现是目标）；键来自外部输入时请自带键控哈希

## 目录结构

```
src/index_map.hpp               核心库（单头文件）
tests/test_index_map.cpp        单元测试（15 用例 / 5.6 万断言，含随机模型对照）
benchmarks/bench_index_map.cpp  性能对比
docs/knowledge-points.md        原文知识点完整提炼（设计依据）
output/benchmark-result.txt     基准原始输出
```
