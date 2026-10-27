// test_index_map.cpp — IndexMap 单元测试
// 覆盖意图（每条测试注明它保护的语义/不变量，来源见 docs/knowledge-points.md）：
//   * 插入序保真（本库存在的理由）
//   * entries 无空洞不变量（get_index O(1) 的前提）——每次变更后全量重查验证
//   * swap/shift 删除的下标修正正确性（含同哈希冲突、扩容后场景）
//   * 小容量捷径（len==1 查找不算哈希）
//   * 异构查找、坏哈希器（全碰撞）下的正确性

#include "../src/index_map.hpp"

#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

static int g_failed = 0, g_total = 0;
#define CHECK(cond) do { ++g_total; if (!(cond)) { ++g_failed; \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_total; auto va_ = (a); auto vb_ = (b); \
    if (!(va_ == vb_)) { ++g_failed; \
    std::printf("  FAIL %s:%d: %s == %s (实际 %lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                (long long)va_, (long long)vb_); } } while (0)
#define RUN(fn) do { std::printf("[ RUN      ] %s\n", #fn); \
    int base_ = g_failed; fn(); \
    std::printf(g_failed > base_ ? "[  FAILED  ] %s\n" : "[       OK ] %s\n", #fn); } while (0)

using im::IndexMap;

// 计数哈希器：验证"单元素查找不算哈希"的捷径意图
struct CountingHash {
    mutable std::size_t calls = 0;
    std::size_t operator()(std::uint64_t k) const { calls++; return (std::size_t)k; }
};
// 坏哈希器：全部同 hash（0）——SwissTable tag 全碰撞，走 eq 慢路径的极端情形
struct AllSameHash {
    std::size_t operator()(std::uint64_t) const { return 0; }
};

// 不变量校验：entries 与索引表完全一致（无空洞 + 每键可查 + 下标正确）
void check_consistent(IndexMap<std::uint64_t, int>& m,
                      const std::vector<std::uint64_t>& expect_keys_in_order) {
    CHECK_EQ(m.size(), expect_keys_in_order.size());
    std::size_t i = 0;
    for (auto [k, v] : m) {  // 迭代序 == 期望插入序
        CHECK_EQ(k, expect_keys_in_order[i]);
        (void)v;
        ++i;
    }
    for (std::uint64_t k : expect_keys_in_order) {  // 每键可查且下标正确
        CHECK(m.find(k) != nullptr);
        CHECK(m.get_index_of(k) != m.npos);
        CHECK_EQ(m.key_at(m.get_index_of(k)), k);
    }
}

// ---------- 1. 插入保序 ----------
static void test_insert_preserves_order() {
    IndexMap<std::uint64_t, int> m;
    for (std::uint64_t k : {100ull, 7ull, 55ull, 3ull, 42ull}) m.insert(k, (int)k);
    std::uint64_t expect[] = {100, 7, 55, 3, 42};
    std::size_t i = 0;
    for (auto [k, v] : m) { CHECK_EQ(k, expect[i]); CHECK_EQ(v, (int)expect[i]); ++i; }
    CHECK_EQ(i, (std::size_t)5);
}

// ---------- 2. 重复键：原位替换，位置不动 ----------
static void test_existing_key_updates_in_place() {
    IndexMap<std::uint64_t, int> m;
    auto [i1, ins1] = m.insert_full(10, 1);
    auto [i2, ins2] = m.insert_full(20, 2);
    auto [i3, ins3] = m.insert_full(10, 99);   // 重复
    CHECK(ins1 && ins2 && !ins3);
    CHECK_EQ(i1, (std::size_t)0); CHECK_EQ(i2, (std::size_t)1); CHECK_EQ(i3, (std::size_t)0);
    CHECK_EQ(m.size(), (std::size_t)2);
    CHECK_EQ(*m.find(10), 99);
    CHECK_EQ(m.key_at(0), 10ull);              // 位置不动
    CHECK_EQ(m.value_at(0), 99);
    CHECK_EQ(m.key_at(1), 20ull);
}

// ---------- 3. swap_remove：末元素补位，其余不动 ----------
static void test_swap_remove_semantics() {
    IndexMap<std::uint64_t, int> m;
    for (std::uint64_t k : {1ull, 2ull, 3ull, 4ull, 5ull}) m.insert(k, (int)k);
    auto removed = m.swap_remove(2);           // 删下标 1
    CHECK(removed.has_value());
    CHECK_EQ(removed->first, 2ull); CHECK_EQ(removed->second, 2);
    // 期望顺序：1, 5, 3, 4（末元素 5 补位）
    std::uint64_t expect[] = {1, 5, 3, 4};
    std::size_t i = 0;
    for (auto [k, v] : m) { CHECK_EQ(k, expect[i]); (void)v; ++i; }
    CHECK_EQ(m.get_index_of(5), (std::size_t)1);   // 被移动元素的下标已修正
    CHECK_EQ(m.get_index_of(3), (std::size_t)2);
    CHECK(!m.contains(2));
    check_consistent(m, {1, 5, 3, 4});
    // 删尾（无元素移动的路径）
    auto r2 = m.swap_remove(4);
    CHECK(r2.has_value());
    check_consistent(m, {1, 5, 3});
}

// ---------- 4. shift_remove：保序 ----------
static void test_shift_remove_semantics() {
    IndexMap<std::uint64_t, int> m;
    for (std::uint64_t k : {1ull, 2ull, 3ull, 4ull, 5ull}) m.insert(k, (int)k);
    auto removed = m.shift_remove(1);
    CHECK(removed.has_value());
    check_consistent(m, {2, 3, 4, 5});
    CHECK_EQ(m.get_index_of(5), (std::size_t)3);
    // 下标 0 删除：最坏情形（后段全移）
    m.shift_remove_index(0);
    check_consistent(m, {3, 4, 5});
    CHECK(!m.shift_remove(999).has_value());   // 未命中
}

// ---------- 5. pop / first / last ----------
static void test_pop_and_endpoints() {
    IndexMap<std::uint64_t, int> m;
    CHECK(!m.pop().has_value());               // 空 pop 安全
    m.insert(1, 10); m.insert(2, 20);
    CHECK_EQ(m.first().key, 1ull); CHECK_EQ(m.first().value, 10);
    CHECK_EQ(m.last().key, 2ull);  CHECK_EQ(m.last().value, 20);
    auto p = m.pop();
    CHECK(p.has_value());
    CHECK_EQ(p->first, 2ull); CHECK_EQ(p->second, 20);
    check_consistent(m, {1});
}

// ---------- 6. 排序后重建索引表（用缓存 hash，不重算） ----------
static void test_sort_keys_then_append() {
    IndexMap<std::uint64_t, int> m;
    for (std::uint64_t k : {5ull, 1ull, 4ull, 2ull, 3ull}) m.insert(k, (int)k);
    m.sort_keys();
    check_consistent(m, {1, 2, 3, 4, 5});
    m.insert(0, 0);                            // 排序后新键仍追加末尾
    m.insert(3, 33);                           // 旧键原位替换
    check_consistent(m, {1, 2, 3, 4, 5, 0});
    CHECK_EQ(m.value_at(2), 33);
}

// ---------- 7. retain：无删除不重建；有删除保序 ----------
static void test_retain() {
    IndexMap<std::uint64_t, int> m;
    for (std::uint64_t k = 0; k < 10; ++k) m.insert(k, (int)k);
    CHECK_EQ(m.retain([](std::uint64_t, int) { return true; }), (std::size_t)0);  // 全保留
    CHECK_EQ(m.retain([](std::uint64_t k, int) { return k % 2 == 0; }), (std::size_t)5);
    check_consistent(m, {0, 2, 4, 6, 8});      // 保序
}

// ---------- 8. move_index / swap_indices ----------
static void test_move_and_swap_indices() {
    IndexMap<std::uint64_t, int> m;
    for (std::uint64_t k = 0; k < 6; ++k) m.insert(k, (int)k);
    m.move_index(0, 3);                        // 0 1 2 3 4 5 → 1 2 3 0 4 5
    check_consistent(m, {1, 2, 3, 0, 4, 5});
    m.move_index(5, 0);                        // → 5 1 2 3 0 4
    check_consistent(m, {5, 1, 2, 3, 0, 4});
    m.swap_indices(0, 4);                      // → 0 1 2 3 5 4
    check_consistent(m, {0, 1, 2, 3, 5, 4});
    m.swap_indices(1, 1);                      // 自身交换无操作
    check_consistent(m, {0, 1, 2, 3, 5, 4});
}

// ---------- 9. shift_insert ----------
static void test_shift_insert() {
    IndexMap<std::uint64_t, int> m;
    for (std::uint64_t k : {1ull, 2ull, 3ull}) m.insert(k, (int)k);
    m.shift_insert(1, 99, 990);                // 1 99 2 3
    check_consistent(m, {1, 99, 2, 3});
    CHECK_EQ(m.get_index_of(2), (std::size_t)2);
    m.shift_insert(0, 77, 770);
    check_consistent(m, {77, 1, 99, 2, 3});
    m.shift_insert(m.size(), 55, 550);         // 末尾插入
    check_consistent(m, {77, 1, 99, 2, 3, 55});
}

// ---------- 10. 单元素捷径：len==1 查找不算哈希 ----------
static void test_single_element_shortcut() {
    CountingHash h;
    IndexMap<std::uint64_t, int, CountingHash> m(h);
    m.insert(42, 1);
    const std::size_t calls_after_insert = h.calls;
    CHECK(m.contains(42));                     // 捷径：0 次哈希
    CHECK(!m.contains(43));
    CHECK_EQ(h.calls, calls_after_insert);
    CHECK(m.find(42) != nullptr);
    CHECK_EQ(h.calls, calls_after_insert);
    CHECK(m.get_index_of(43) == m.npos);
    CHECK(!m.shift_remove(43).has_value());    // 未命中删除不动表
    CHECK_EQ(h.calls, calls_after_insert);
}

// ---------- 11. 异构查找（string_view 查 string 键） ----------
static void test_heterogeneous_find() {
    IndexMap<std::string, int> m;
    m.insert(std::string("alpha"), 1);
    m.insert(std::string("beta"), 2);
    auto hash_sv = [](std::string_view s) {
        return std::hash<std::string_view>{}(s);   // 须与 hash(string) 同值
    };
    auto eq_sv = [](std::string_view q, const std::string& k) { return q == k; };
    int* v = m.find_equiv(std::string_view("beta"), hash_sv, eq_sv);
    CHECK(v != nullptr);
    if (v) CHECK_EQ(*v, 2);
    CHECK(m.find_equiv(std::string_view("gamma"), hash_sv, eq_sv) == nullptr);
    CHECK_EQ(m.get_index_of_equiv(std::string_view("alpha"), hash_sv, eq_sv), (std::size_t)0);
}

// ---------- 12. 坏哈希器：全碰撞仍正确 ----------
static void test_bad_hash_all_same() {
    IndexMap<std::uint64_t, int, AllSameHash> m;
    const int N = 300;                          // 超过单表容量，强制扩容 + 全碰撞
    for (int i = 0; i < N; ++i) m.insert((std::uint64_t)i, i * 10);
    CHECK_EQ(m.size(), (std::size_t)N);
    for (int i = 0; i < N; ++i) CHECK_EQ(*m.find((std::uint64_t)i), i * 10);
    for (int i = 0; i < N; i += 3) CHECK(m.shift_remove((std::uint64_t)i).has_value());
    CHECK_EQ(m.size(), (std::size_t)(N - (N + 2) / 3));
    for (int i = 0; i < N; ++i) {
        bool hit = m.find((std::uint64_t)i) != nullptr;
        CHECK_EQ(hit, i % 3 != 0);
    }
    // 顺序保持：剩余键为非 3 倍数
    std::size_t expect = 0;
    for (auto [k, v] : m) {
        while (expect % 3 == 0) ++expect;
        CHECK_EQ(k, expect);
        (void)v;
        ++expect;
    }
    CHECK_EQ(expect, (std::size_t)N);          // 最大保留键 299（3 的倍数已删）
}

// ---------- 13. 随机模型对照（vs std::unordered_map） ----------
static void test_randomized_model() {
    std::mt19937_64 rng(20261027);
    for (int trial = 0; trial < 3; ++trial) {
        IndexMap<std::uint64_t, int> m;
        std::unordered_map<std::uint64_t, int> model;
        std::vector<std::uint64_t> order;       // m 的期望插入序
        const int OPS = 4000;
        for (int op = 0; op < OPS; ++op) {
            int act = (int)(rng() % 100);
            std::uint64_t k = rng() % 500;      // 小键空间制造大量重复插入/删除
            if (act < 45) {                     // 插入或替换
                int v = (int)(rng() % 1000);
                bool existed = model.count(k) != 0;
                model[k] = v;
                if (!existed) order.push_back(k);
                auto [idx, ins] = m.insert_full(k, v);
                CHECK_EQ(ins, !existed);
                CHECK_EQ(m.value_at(idx), v);
            } else if (act < 70) {              // swap_remove（模型侧同步换序）
                bool existed = model.count(k) != 0;
                auto r = m.swap_remove(k);
                CHECK_EQ(r.has_value(), existed);
                if (existed) {
                    model.erase(k);
                    auto it = std::find(order.begin(), order.end(), k);
                    *it = order.back();         // 末元素补位
                    order.pop_back();
                }
            } else if (act < 85) {              // shift_remove（模型侧保序删除）
                bool existed = model.count(k) != 0;
                auto r = m.shift_remove(k);
                CHECK_EQ(r.has_value(), existed);
                if (existed) {
                    model.erase(k);
                    order.erase(std::find(order.begin(), order.end(), k));
                }
            } else {                            // 查找
                auto* p = m.find(k);
                CHECK_EQ(p != nullptr, model.count(k) != 0);
                if (p) CHECK_EQ(*p, model[k]);
            }
            if (op % 500 == 0) check_consistent(m, order);  // 阶段性全量校验
        }
        check_consistent(m, order);
        CHECK_EQ(m.size(), model.size());
        for (auto& [k, v] : model) CHECK_EQ(*m.find(k), v);  // 终态逐键对照
    }
}

// ---------- 14. 扩容循环：插满删空多轮（tombstone + rehash 路径） ----------
static void test_grow_shrink_cycles() {
    IndexMap<std::uint64_t, int> m;
    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < 1000; ++i) m.insert((std::uint64_t)i + round * 100000, i);
        CHECK_EQ(m.size(), (std::size_t)1000);
        for (int i = 0; i < 1000; i += 2) CHECK(m.swap_remove((std::uint64_t)i + round * 100000).has_value());
        for (int i = 1; i < 1000; i += 2) CHECK(m.shift_remove((std::uint64_t)i + round * 100000).has_value());
        CHECK(m.empty());
        CHECK(!m.pop().has_value());
    }
    m.reserve(5000);                            // 预留后一次到位
    for (int i = 0; i < 5000; ++i) m.insert((std::uint64_t)i, i);
    for (int i = 0; i < 5000; ++i) CHECK_EQ(*m.find((std::uint64_t)i), i);
}

// ---------- 15. 迭代器与移动语义 ----------
static void test_iterators_and_move() {
    IndexMap<std::uint64_t, int> a;
    for (std::uint64_t k = 0; k < 100; ++k) a.insert(k, (int)k);
    const auto& ca = a;
    std::uint64_t sum_k = 0; int sum_v = 0;
    for (auto&& [k, v] : ca) { sum_k += k; sum_v += v; }
    CHECK_EQ(sum_k, 99ull * 100 / 2);
    CHECK_EQ(sum_v, 99 * 100 / 2);
    IndexMap<std::uint64_t, int> b(std::move(a));   // 移动构造
    CHECK(a.empty());
    CHECK_EQ(b.size(), (std::size_t)100);
    CHECK_EQ(*b.find(55), 55);
    b.insert(1000, 1);
    CHECK_EQ(b.size(), (std::size_t)101);
    for (auto [k, v] : b) { (void)k; v += 1; }  // 代理引用可写
    CHECK_EQ(*b.find(0), 1);
}

int main() {
    std::printf("IndexMap 单元测试\n==================\n");
    RUN(test_insert_preserves_order);
    RUN(test_existing_key_updates_in_place);
    RUN(test_swap_remove_semantics);
    RUN(test_shift_remove_semantics);
    RUN(test_pop_and_endpoints);
    RUN(test_sort_keys_then_append);
    RUN(test_retain);
    RUN(test_move_and_swap_indices);
    RUN(test_shift_insert);
    RUN(test_single_element_shortcut);
    RUN(test_heterogeneous_find);
    RUN(test_bad_hash_all_same);
    RUN(test_randomized_model);
    RUN(test_grow_shrink_cycles);
    RUN(test_iterators_and_move);
    std::printf("------------------\n%d 项断言，%d 失败\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
