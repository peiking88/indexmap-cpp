// bench_index_map.cpp — IndexMap vs std::unordered_map vs std::map 性能对比
// 方法论对齐 docs/knowledge-points.md §5（原文基准：插入/查找/迭代/删除后迭代/swap_remove）
// 计时：steady_clock，每场景多轮取中位数；累加结果作防优化 sink。

#include "../src/index_map.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

static std::uint64_t g_sink = 0;  // 防优化累积

// 混合哈希器（splitmix64 finalizer）：三容器统一使用。
// 原因：std::hash<u64> 恒等映射，键 < 2^57 时 SwissTable tag（hash 高 7 位）全为 0，
// tag 过滤失效（知识点 §9：整数键须先位混淆；文章对照用 FxBuildHasher 同理）。
struct MixHash {
    std::size_t operator()(std::uint64_t x) const {
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return static_cast<std::size_t>(x ^ (x >> 31));
    }
};
using UMap = std::unordered_map<std::uint64_t, std::uint64_t, MixHash>;
using IMap = im::IndexMap<std::uint64_t, std::uint64_t, MixHash>;

template <class F>
static double median_ms(F&& f, int reps) {
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(reps));
    for (int r = 0; r < reps; ++r) {
        const auto t0 = Clock::now();
        g_sink += f();
        const auto t1 = Clock::now();
        t.push_back(std::chrono::duration_cast<ns>(t1 - t0).count() / 1e6);
    }
    std::sort(t.begin(), t.end());
    return t[static_cast<std::size_t>(reps / 2)];
}

template <class Map>
static void fill(Map& m, const std::vector<std::uint64_t>& keys) {
    for (auto k : keys) m.insert({k, k * 3});
}
static void fill(IMap& m, const std::vector<std::uint64_t>& keys) {
    for (auto k : keys) m.insert(k, k * 3);
}

int main() {
    std::printf("IndexMap vs std::unordered_map vs std::map（u64→u64，-O2 -march=native，中位数）\n");
    std::printf("==========================================================================\n");

    for (std::uint64_t N : {1000ull, 100000ull, 1000000ull}) {
        const int reps = N <= 1000 ? 20 : (N <= 100000 ? 5 : 3);
        std::vector<std::uint64_t> keys(static_cast<std::size_t>(N));
        for (std::uint64_t i = 0; i < N; ++i) keys[static_cast<std::size_t>(i)] = i;
        std::vector<std::uint64_t> shuffled = keys;
        { std::mt19937_64 rng(42); std::shuffle(shuffled.begin(), shuffled.end(), rng); }
        // 未命中键集：[N, 2N) 每 7 个取一
        std::vector<std::uint64_t> missing;
        for (std::uint64_t i = N; i < 2 * N; i += 7) missing.push_back(i);

        std::printf("\n---- N = %llu ----\n", (unsigned long long)N);

        // ---------- 插入（顺序键，从空） ----------
        {
            double a = median_ms([&] {
                IMap m;
                for (auto k : keys) m.insert(k, k * 3);
                return m.size();
            }, reps);
            double b = median_ms([&] {
                UMap m;
                m.reserve(static_cast<std::size_t>(N));
                for (auto k : keys) m.insert({k, k * 3});
                return m.size();
            }, reps);
            double c = median_ms([&] {
                std::map<std::uint64_t, std::uint64_t> m;
                for (auto k : keys) m.insert({k, k * 3});
                return m.size();
            }, reps);
            std::printf("插入           IM %8.2fms | umap %8.2fms (x%.2f) | map %8.2fms (x%.2f)\n",
                        a, b, b / a, c, c / a);
        }

        // ---------- 命中查找（打乱键全查） ----------
        {
            IMap im;
            UMap um;
            um.reserve(static_cast<std::size_t>(N));
            std::map<std::uint64_t, std::uint64_t> sm;
            fill(im, keys); fill(um, keys); fill(sm, keys);
            double a = median_ms([&] {
                std::uint64_t s = 0;
                for (auto k : shuffled) s += *im.find(k);
                return s;
            }, reps);
            double b = median_ms([&] {
                std::uint64_t s = 0;
                for (auto k : shuffled) s += um.find(k)->second;
                return s;
            }, reps);
            double c = median_ms([&] {
                std::uint64_t s = 0;
                for (auto k : shuffled) s += sm.find(k)->second;
                return s;
            }, reps);
            std::printf("命中查找       IM %8.2fms | umap %8.2fms (x%.2f) | map %8.2fms (x%.2f)\n",
                        a, b, b / a, c, c / a);
        }

        // ---------- 未命中查找 ----------
        {
            IMap im;
            UMap um;
            um.reserve(static_cast<std::size_t>(N));
            fill(im, keys); fill(um, keys);
            double a = median_ms([&] {
                std::uint64_t s = 0;
                for (auto k : missing) s += (im.find(k) != nullptr);
                return s;
            }, reps);
            double b = median_ms([&] {
                std::uint64_t s = 0;
                for (auto k : missing) s += (um.find(k) != um.end());
                return s;
            }, reps);
            std::printf("未命中查找     IM %8.2fms | umap %8.2fms (x%.2f)\n", a, b, b / a);
        }

        // ---------- 迭代（全量求和） ----------
        {
            IMap im;
            UMap um;
            um.reserve(static_cast<std::size_t>(N));
            std::map<std::uint64_t, std::uint64_t> sm;
            fill(im, keys); fill(um, keys); fill(sm, keys);
            double a = median_ms([&] {
                std::uint64_t s = 0;
                for (auto&& [k, v] : im) s += k + v;
                return s;
            }, reps);
            double b = median_ms([&] {
                std::uint64_t s = 0;
                for (auto& kv : um) s += kv.first + kv.second;
                return s;
            }, reps);
            double c = median_ms([&] {
                std::uint64_t s = 0;
                for (auto& kv : sm) s += kv.first + kv.second;
                return s;
            }, reps);
            std::printf("迭代           IM %8.2fms | umap %8.2f (IM 快 x%.2f) | map %8.2f (IM 快 x%.2f)\n",
                        a, b, b / a, c, c / a);
        }

        // ---------- 删除 90% 后迭代 ----------
        {
            IMap im;
            UMap um;
            um.reserve(static_cast<std::size_t>(N));
            std::map<std::uint64_t, std::uint64_t> sm;
            fill(im, keys); fill(um, keys); fill(sm, keys);
            const std::uint64_t step = 10;      // 每组 10 个删前 9 个 → 删 90%
            for (std::uint64_t k = 0; k < N; k += step)
                for (std::uint64_t j = 0; j + 1 < step && k + j < N; ++j) {
                    im.swap_remove(k + j);
                    um.erase(k + j);
                    sm.erase(k + j);
                }
            double a = median_ms([&] {
                std::uint64_t s = 0;
                for (auto&& [k, v] : im) s += k + v;
                return s;
            }, reps);
            double b = median_ms([&] {
                std::uint64_t s = 0;
                for (auto& kv : um) s += kv.first + kv.second;
                return s;
            }, reps);
            double c = median_ms([&] {
                std::uint64_t s = 0;
                for (auto& kv : sm) s += kv.first + kv.second;
                return s;
            }, reps);
            std::printf("删90%%后迭代    IM %8.2fms | umap %8.2f (IM 快 x%.2f) | map %8.2f (IM 快 x%.2f)\n",
                        a, b, b / a, c, c / a);
        }

        // ---------- swap_remove / erase 全量乱序删除 ----------
        {
            double a = median_ms([&] {
                IMap m;
                fill(m, keys);
                std::uint64_t s = 0;
                for (auto k : shuffled) s += m.swap_remove(k).has_value();
                return s;
            }, reps);
            double b = median_ms([&] {
                UMap m;
                m.reserve(static_cast<std::size_t>(N));
                fill(m, keys);
                std::uint64_t s = 0;
                for (auto k : shuffled) s += m.erase(k);
                return s;
            }, reps);
            std::printf("swap_remove    IM %8.2fms | umap erase %8.2fms (x%.2f)\n", a, b, b / a);
        }

        // ---------- shift_remove(0)（保序删除的代价，每次删当前首元素） ----------
        if (N <= 100000) {
            const std::uint64_t K = 1000;  // 固定删 1000 个控制时长
            IMap im;
            fill(im, keys);
            double a = median_ms([&] {
                for (std::uint64_t i = 0; i < K; ++i) im.shift_remove_index(0);
                return im.size();
            }, 1);
            UMap um;
            um.reserve(static_cast<std::size_t>(N));
            fill(um, keys);
            double b = median_ms([&] {
                for (std::uint64_t i = 0; i < K; ++i) um.erase(um.begin());
                return um.size();
            }, 1);
            std::printf("shift_remove0  IM %8.2fms | umap erase(begin) %8.2fms（IM 为保序 O(n) 代价）\n", a, b);
        }

        // ---------- 内存估算 ----------
        {
            IMap im;
            fill(im, keys);
            UMap um;
            um.reserve(static_cast<std::size_t>(N));
            fill(um, keys);
            const std::size_t im_bytes = im.entries_memory_bytes() + im.index_table_memory_bytes();
            // unordered_map: 桶指针数组 + 节点( next 指针 8 + kv 16 + malloc 头 ~16 )
            const std::size_t um_bytes = um.bucket_count() * 8 + um.size() * 40;
            std::printf("内存估算       IM %6.1f MB (%zu B/元素) | umap %6.1f MB (%zu B/元素)\n",
                        im_bytes / 1e6, im_bytes / im.size(),
                        um_bytes / 1e6, um_bytes / um.size());
        }
    }

    std::printf("\n[sink = %llu]\n", (unsigned long long)g_sink);
    return 0;
}
