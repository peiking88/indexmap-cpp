// index_map.hpp — 高性能保插入序哈希映射（Rust indexmap 的 C++ 移植）
//
// 设计依据：docs/knowledge-points.md（来源文章《深入 Rust IndexMap》）
//   entries : 稠密数组（Bucket{hash, key, value}，插入序、无空洞）→ O(1) 下标访问
//   indices : SwissTable（控制字节 + SSE2 组探测 + 三角探测，槽位存 entries 下标）
//
// 删除 API 有意二分（对齐文章的设计教训：显式暴露代价）：
//   swap_remove  O(1)  末元素补位，不保序
//   shift_remove O(n)  保序
// 本库不提供含糊的 remove()。
//
// 线程安全性：与 std::vector / std::unordered_map 相同——并发读安全，读写需外部同步。
// 迭代器失效：与 std::vector 相同——插入可能使全部迭代器失效（扩容）；
//   swap_remove 使被移动元素的下标失效引用；其余下标/引用稳定。

#pragma once

#define INDEXMAP_VERSION "1.0.0"
#define INDEXMAP_VERSION_MAJOR 1
#define INDEXMAP_VERSION_MINOR 0
#define INDEXMAP_VERSION_PATCH 0

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace im {

 //---------------------------------------------------------------
 // SwissTable 常量与 SIMD 组原语
 //---------------------------------------------------------------
inline constexpr std::size_t kGroupWidth = 16;    // 一组 16 个控制字节
inline constexpr std::uint8_t kEmptyTag = 0xFF;   // 空槽（全 1）
inline constexpr std::uint8_t kDeletedTag = 0x80; // 墓碑；合法 tag 为 hash 高 7 位(0x00..0x7F)，高位恒 0，不与特殊值冲突
inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;  // entries 下标上限 2^32-2（哨兵 + npos）

#if defined(__GNUC__)
#define IM_LIKELY(x) __builtin_expect(!!(x), 1)
#define IM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define IM_LIKELY(x) (x)
#define IM_UNLIKELY(x) (x)
#endif

// hash 高 7 位作 tag
inline std::uint8_t ctrl_tag(std::uint64_t h) { return static_cast<std::uint8_t>(h >> 57); }

// full 槽判定：tag 恒 < 0x80，EMPTY/DELETED 高位为 1（一次比较）
inline bool ctrl_is_full(std::uint8_t c) { return (c & 0x80u) == 0; }

inline std::uint32_t ctz32(std::uint32_t x) {
#if defined(_MSC_VER)
    unsigned long r; _BitScanForward(&r, x); return static_cast<std::uint32_t>(r);
#else
    return static_cast<std::uint32_t>(__builtin_ctz(x));
#endif
}

#if defined(__SSE2__)
// 组一次加载、多谓词复用（省同地址二次 loadu）
using GroupVec = __m128i;
inline GroupVec group_load(const std::uint8_t* ctrl) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(ctrl));
}
// 位图 bit i = (ctrl[i] == tag)
inline std::uint32_t group_match_tag(GroupVec g, std::uint8_t tag) {
    return static_cast<std::uint32_t>(_mm_movemask_epi8(
        _mm_cmpeq_epi8(g, _mm_set1_epi8(static_cast<char>(tag)))));
}
// 位图 bit i = (ctrl[i] == EMPTY)
inline std::uint32_t group_match_empty(GroupVec g) {
    return static_cast<std::uint32_t>(_mm_movemask_epi8(
        _mm_cmpeq_epi8(g, _mm_set1_epi8(static_cast<char>(kEmptyTag)))));
}
// 位图 bit i = (ctrl[i] 为 EMPTY 或 DELETED)，即"可插入槽"（有符号 0 > byte ⟺ 高位为 1）
inline std::uint32_t group_match_empty_or_deleted(GroupVec g) {
    return static_cast<std::uint32_t>(_mm_movemask_epi8(
        _mm_cmpgt_epi8(_mm_setzero_si128(), g)));
}
#else
// 标量回退：组即指针
using GroupVec = const std::uint8_t*;
inline GroupVec group_load(const std::uint8_t* ctrl) { return ctrl; }
inline std::uint32_t group_match_tag(GroupVec g, std::uint8_t tag) {
    std::uint32_t bits = 0;
    for (std::uint32_t i = 0; i < kGroupWidth; ++i)
        if (g[i] == tag) bits |= 1u << i;
    return bits;
}
inline std::uint32_t group_match_empty(GroupVec g) {
    return group_match_tag(g, kEmptyTag);
}
inline std::uint32_t group_match_empty_or_deleted(GroupVec g) {
    std::uint32_t bits = 0;
    for (std::uint32_t i = 0; i < kGroupWidth; ++i)
        if ((g[i] & 0x80u) != 0) bits |= 1u << i;
    return bits;
}
#endif

 //---------------------------------------------------------------
 // SwissIndexTable — 只存 uint32 下标的开地址哈希表
 //   probe: pos = h & mask; pos += stride (16,32,48..)（三角探测）
 //   删除置 DELETED（墓碑）；growth 耗尽时：墓碑多则原位重排，真满则倍增。
 //   恒保证 EMPTY 槽 >= buckets/8（探测必然终止）。
 //---------------------------------------------------------------
class SwissIndexTable {
public:
    using Slot = std::uint32_t;

    SwissIndexTable() = default;
    ~SwissIndexTable() { destroy(); }
    SwissIndexTable(const SwissIndexTable&) = delete;
    SwissIndexTable& operator=(const SwissIndexTable&) = delete;
    SwissIndexTable(SwissIndexTable&& o) noexcept { move_from(o); }
    SwissIndexTable& operator=(SwissIndexTable&& o) noexcept {
        if (this != &o) { destroy(); move_from(o); }
        return *this;
    }

    std::size_t size() const { return size_; }
    std::size_t buckets() const { return bucket_mask_ + 1; }   // 槽位数
    std::size_t capacity() const { return buckets() / 8 * 7; } // 可容纳元素数（7/8 负载）
    bool allocated() const { return ctrl_ != nullptr; }

    // 在表中查找：eq(slot_value) 判定命中（slot_value 为槽中存的 entries 下标）。
    // 返回槽中的下标；未命中返回 kInvalidIndex。
    template <class Eq>
    Slot find(std::uint64_t hash, Eq eq) const {
        const std::size_t pos = find_slot(hash, eq);
        return pos == kNpos ? kInvalidIndex : slots_[pos];
    }

    // 插入新下标（调用方保证该 key 尚不存在）。hash_of(slot_value) 取该下标对应条目的
    // 完整 hash（扩容/原位重排无需重算——entries Bucket 中已缓存）。
    template <class HashOf>
    void insert_new(std::uint64_t hash, Slot index, HashOf hash_of) {
        if (IM_UNLIKELY(!allocated() || growth_left_ == 0)) {
            if (size_ < capacity()) rehash_in_place(hash_of);
            else grow(buckets() * 2, hash_of);
        }
        insert_no_grow(hash, index);
    }

    // 删除：eq(slot_value) 判定目标槽。返回是否删除成功。
    template <class Eq>
    bool erase(std::uint64_t hash, Eq eq) {
        const std::size_t pos = find_slot(hash, eq);
        if (pos == kNpos) return false;
        set_ctrl(pos, kDeletedTag);
        --size_;  // DELETED 仍是非 EMPTY 槽：growth_left_ 不变
        return true;
    }

    // 找到槽并改写其值。eq(slot_value) 命中即改（调用方保证目标值唯一）。
    template <class Eq>
    bool update_value(std::uint64_t hash, Slot new_value, Eq eq) {
        const std::size_t pos = find_slot(hash, eq);
        if (pos == kNpos) return false;
        slots_[pos] = new_value;
        return true;
    }

    // shift_remove 的全表扫描路径：槽值 ∈ [begin, end) 的都减 1（entries 区间左移一格）。
    void decrement_range(std::uint32_t begin, std::uint32_t end) {
        scan_range(begin, end, -1);
    }

    // shift_insert 的全表扫描路径：槽值 ∈ [begin, end) 的都加 1。
    void increment_range(std::uint32_t begin, std::uint32_t end) {
        scan_range(begin, end, +1);
    }

    // 清空（保留内存）。rebuild 场景：调用方随后重新 insert_new。
    void clear() {
        if (allocated()) {
            std::memset(ctrl_, kEmptyTag, buckets() + kGroupWidth);
            size_ = 0;
            growth_left_ = capacity();
        }
    }

    // 保证 capacity() >= n（不缩容）。
    template <class HashOf>
    void reserve(std::size_t n, HashOf hash_of) {
        std::size_t want = n <= 8 ? 16 : next_pow2((n * 8 + 6) / 7);
        if (buckets() < want) grow(want, hash_of);
    }

    // 内存字节数（估算，供基准报告）
    std::size_t memory_bytes() const {
        return allocated() ? buckets() * (sizeof(Slot) + 1) + kGroupWidth : 0;
    }

private:
    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    // 单块内存布局：[ctrl: buckets+kGroupWidth 字节][slots: buckets*4 字节]
    // 一次分配/释放、TLB 与预取友好；slots 偏移为 16 的倍数 → 天然对齐。
    std::uint8_t* block_ = nullptr; // 整块基址（未分配时 nullptr）
    std::uint8_t* ctrl_ = nullptr;  // = block_，尾部复制首组（环形组加载）
    Slot* slots_ = nullptr;         // = block_ + buckets + kGroupWidth
    std::size_t bucket_mask_ = 0;   // buckets - 1（0 表示未分配）
    std::size_t size_ = 0;
    std::size_t growth_left_ = 0;   // capacity() - (FULL+DELETED 槽数)

    static std::size_t next_pow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    void set_ctrl(std::size_t pos, std::uint8_t b) {
        ctrl_[pos] = b;
        if (pos < kGroupWidth) ctrl_[pos + buckets()] = b;  // 维护首组镜像
    }

    template <class Eq>
    std::size_t find_slot(std::uint64_t hash, Eq eq) const {
        if (bucket_mask_ == 0) return kNpos;
        const std::uint8_t tag = ctrl_tag(hash);
        std::size_t pos = static_cast<std::size_t>(hash) & bucket_mask_;
        std::size_t stride = 0;
        for (;;) {
            const GroupVec g = group_load(ctrl_ + pos);  // 单次加载，tag/empty 两用
            std::uint32_t bits = group_match_tag(g, tag);
            while (bits) {
                const std::size_t slot = (pos + ctz32(bits)) & bucket_mask_;
                if (eq(slots_[slot])) return slot;
                bits &= bits - 1;
            }
            if (IM_LIKELY(group_match_empty(g) != 0)) return kNpos;  // 首组即终止占绝大多数
            stride += kGroupWidth;
            pos = (pos + stride) & bucket_mask_;
        }
    }

    // 分配新表并把旧表 FULL 槽迁入（或首次分配）。
    template <class HashOf>
    void grow(std::size_t new_buckets, HashOf hash_of) {
        if (new_buckets < 16) new_buckets = 16;
        std::uint8_t* new_block = new std::uint8_t[new_buckets + kGroupWidth + new_buckets * sizeof(Slot)];
        std::uint8_t* new_ctrl = new_block;
        Slot* new_slots = reinterpret_cast<Slot*>(new_block + new_buckets + kGroupWidth);
        std::memset(new_ctrl, kEmptyTag, new_buckets + kGroupWidth);

        const std::size_t old_buckets = buckets();
        std::uint8_t* old_ctrl = ctrl_;
        Slot* old_slots = slots_;
        std::uint8_t* old_block = block_;

        block_ = new_block; ctrl_ = new_ctrl; slots_ = new_slots;
        bucket_mask_ = new_buckets - 1;
        size_ = 0;
        growth_left_ = capacity();

        if (old_ctrl) {  // 首次分配时无旧表可迁移（未分配时 buckets() 伪值为 1）
            for (std::size_t i = 0; i < old_buckets; ++i) {
                if (ctrl_is_full(old_ctrl[i]))
                    insert_no_grow(hash_of(old_slots[i]), old_slots[i]);
            }
        }
        delete[] old_block;
    }

    // 常规插入：假定表有余量或余量刚恢复（insert_new 已处理 growth）
    void insert_no_grow(std::uint64_t hash, Slot index) {
        const std::uint8_t tag = ctrl_tag(hash);
        std::size_t pos = static_cast<std::size_t>(hash) & bucket_mask_;
        std::size_t stride = 0;
        for (;;) {
            const GroupVec g = group_load(ctrl_ + pos);
            std::uint32_t bits = group_match_empty_or_deleted(g);
            if (IM_LIKELY(bits != 0)) {
                const std::size_t slot = (pos + ctz32(bits)) & bucket_mask_;
                if (ctrl_[slot] == kEmptyTag) --growth_left_;  // 占 EMPTY 才耗余量；复用墓碑不耗
                set_ctrl(slot, tag);
                slots_[slot] = index;
                ++size_;
                return;
            }
            stride += kGroupWidth;
            pos = (pos + stride) & bucket_mask_;
        }
    }

    // 墓碑过多导致的 growth 耗尽：容量不变，清墓碑原位重排。
    template <class HashOf>
    void rehash_in_place(HashOf hash_of) {
        std::vector<Slot> items;
        items.reserve(size_);
        for (std::size_t i = 0; i < buckets(); ++i) {
            if (ctrl_is_full(ctrl_[i])) items.push_back(slots_[i]);
        }
        std::memset(ctrl_, kEmptyTag, buckets() + kGroupWidth);
        size_ = 0;
        growth_left_ = capacity();
        for (Slot v : items) insert_no_grow(hash_of(v), v);
    }

    void scan_range(std::uint32_t begin, std::uint32_t end, int delta) {
        for (std::size_t i = 0; i < buckets(); ++i) {
            if (ctrl_is_full(ctrl_[i])) {
                const Slot v = slots_[i];
                if (v >= begin && v < end)
                    slots_[i] = static_cast<Slot>(static_cast<std::int64_t>(v) + delta);
            }
        }
    }

    void destroy() {
        delete[] block_;
        block_ = nullptr; ctrl_ = nullptr; slots_ = nullptr;
        bucket_mask_ = 0; size_ = 0; growth_left_ = 0;
    }
    void move_from(SwissIndexTable& o) noexcept {
        block_ = o.block_; ctrl_ = o.ctrl_; slots_ = o.slots_;
        bucket_mask_ = o.bucket_mask_; size_ = o.size_; growth_left_ = o.growth_left_;
        o.block_ = nullptr; o.ctrl_ = nullptr; o.slots_ = nullptr;
        o.bucket_mask_ = 0; o.size_ = 0; o.growth_left_ = 0;
    }
};

 //---------------------------------------------------------------
 // IndexMap
 //---------------------------------------------------------------
template <class K, class V,
          class Hash = std::hash<K>,
          class KeyEq = std::equal_to<K>>
class IndexMap {
    struct Bucket {
        std::uint64_t hash;
        K key;
        V value;
    };

public:
    using size_type = std::size_t;
    using key_type = K;
    using mapped_type = V;
    using hasher = Hash;
    using key_equal = KeyEq;
    static constexpr std::size_t npos = static_cast<std::size_t>(kInvalidIndex);

    // ---- 元素视图（迭代产出 & 端点访问）：一对引用 ----
    template <bool IsConst>
    struct EntryView {
        std::conditional_t<IsConst, const K&, K&> key;
        std::conditional_t<IsConst, const V&, V&> value;
    };

    template <bool IsConst>
    class Iterator {
        using BaseIt = std::conditional_t<IsConst,
            typename std::vector<Bucket>::const_iterator,
            typename std::vector<Bucket>::iterator>;
        BaseIt it_;
        friend class IndexMap;
        explicit Iterator(BaseIt it) : it_(it) {}
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = EntryView<IsConst>;
        using reference = value_type;  // 代理：pair-of-references
        using pointer = void;
        using difference_type = std::ptrdiff_t;

        reference operator*() const { return {it_->key, it_->value}; }
        Iterator& operator++() { ++it_; return *this; }
        Iterator operator++(int) { auto t = *this; ++it_; return t; }
        bool operator==(const Iterator& o) const { return it_ == o.it_; }
        bool operator!=(const Iterator& o) const { return it_ != o.it_; }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    IndexMap() = default;
    explicit IndexMap(const Hash& h, const KeyEq& e = KeyEq()) : hash_(h), eq_(e) {}

    // ---- 容量 ----
    size_type size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    void reserve(size_type n) {
        indices_.reserve(n, [this](std::uint32_t i) { return entries_[i].hash; });
        entries_.reserve(indices_.capacity());  // 容量对齐（文章 3.2：避免 Vec 反复扩容）
    }
    void clear() {
        entries_.clear();
        indices_.clear();
    }

    // ---- 查找 ----
    V* find(const K& k) { return find_ptr(*this, k, hash_, eq_); }
    const V* find(const K& k) const { return find_ptr(*this, k, hash_, eq_); }
    bool contains(const K& k) const { return find(k) != nullptr; }

    // 异构查找（文章"Equivalent"知识点）：q 为等价物（如 string_view 查 string 键），
    // qh(q) 须与 hash(等价 key) 同值，qe(q, key) 判等价——不构造完整 key。
    template <class Q, class QHash, class QEq>
    V* find_equiv(const Q& q, QHash qh, QEq qe) { return find_ptr(*this, q, qh, qe); }
    template <class Q, class QHash, class QEq>
    const V* find_equiv(const Q& q, QHash qh, QEq qe) const { return find_ptr(*this, q, qh, qe); }

    // 键 → 插入序下标；未命中返回 npos。
    size_type get_index_of(const K& k) const { return index_of(k, hash_, eq_); }
    template <class Q, class QHash, class QEq>
    size_type get_index_of_equiv(const Q& q, QHash qh, QEq qe) const { return index_of(q, qh, qe); }

    // ---- 下标访问（get_index）----
    const K& key_at(size_type i) const { return entries_[i].key; }
    V& value_at(size_type i) { return entries_[i].value; }
    const V& value_at(size_type i) const { return entries_[i].value; }
    V& operator[](size_type i) { return entries_[i].value; }
    const V& operator[](size_type i) const { return entries_[i].value; }

    EntryView<false> at_index(size_type i) { return {entries_[i].key, entries_[i].value}; }
    EntryView<true> at_index(size_type i) const { return {entries_[i].key, entries_[i].value}; }

    // ---- 端点 ----
    EntryView<false> first() { return {entries_.front().key, entries_.front().value}; }
    EntryView<true> first() const { return {entries_.front().key, entries_.front().value}; }
    EntryView<false> last() { return {entries_.back().key, entries_.back().value}; }
    EntryView<true> last() const { return {entries_.back().key, entries_.back().value}; }

    // ---- 插入 ----
    // 新键追加；旧键原位替换 value（位置不动）。返回 {插入下标, 是否新插入}。
    std::pair<size_type, bool> insert(K k, V v) {
        return insert_full(std::move(k), std::move(v));
    }

    // 同 insert（文章 insert_full 语义：返回下标）。
    std::pair<size_type, bool> insert_full(K k, V v) {
        if (entries_.empty()) {  // 空 → 直接插入（索引表都不用动）
            const std::uint64_t h = hash_(k);
            push_new(h, std::move(k), std::move(v));
            return {0, true};
        }
        const std::uint64_t h = hash_(k);
        const std::size_t idx = find_index_by_hash(h, k);  // 含单元素捷径
        if (idx != npos) {
            entries_[idx].value = std::move(v);  // 原位替换，位置不动
            return {idx, false};
        }
        push_new(h, std::move(k), std::move(v));
        return {size() - 1, true};
    }

    // 指定下标插入（后段右移，O(n)）。前置条件：键不存在（assert）。
    void shift_insert(size_type i, K k, V v) {
        if (i > size()) i = size();
        const std::uint64_t h = hash_(k);
        assert(!contains(k) && "shift_insert: key already present");
        const auto old_size = static_cast<std::uint32_t>(size());
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i),
                        Bucket{h, std::move(k), std::move(v)});
        fix_indices_inserted(static_cast<std::uint32_t>(i), old_size);
        indices_.insert_new(h, static_cast<std::uint32_t>(i),
                            [this](std::uint32_t j) { return entries_[j].hash; });
    }

    // ---- 删除 ----
    // O(1) 删除：末元素补位（不保序）。返回被删的 {key, value}；未命中返回 nullopt。
    std::optional<std::pair<K, V>> swap_remove(const K& k) {
        const std::size_t idx = get_index_of(k);
        if (idx == npos) return std::nullopt;
        return swap_remove_index(idx);
    }
    std::optional<std::pair<K, V>> swap_remove_index(size_type i) {
        const auto last = static_cast<std::uint32_t>(size() - 1);
        const std::uint64_t h = entries_[i].hash;
        // 先删 i 的槽（若先改写 last 槽，值同为 i 的比较会误匹配它）
        indices_.erase(h, [i](std::uint32_t v) { return v == i; });
        std::pair<K, V> out{std::move(entries_[i].key), std::move(entries_[i].value)};
        if (i != last) {
            // 被移动元素：用其缓存 hash 定位（比较下标而非 key——文章 3.4）
            const std::uint64_t mh = entries_[last].hash;
            indices_.update_value(mh, static_cast<std::uint32_t>(i),
                                  [last](std::uint32_t v) { return v == last; });
            entries_[i] = std::move(entries_[last]);
        }
        entries_.pop_back();
        return out;
    }

    // O(n) 保序删除。返回被删的 {key, value}；未命中返回 nullopt。
    std::optional<std::pair<K, V>> shift_remove(const K& k) {
        const std::size_t idx = get_index_of(k);
        if (idx == npos) return std::nullopt;
        return shift_remove_index(idx);
    }
    std::optional<std::pair<K, V>> shift_remove_index(size_type i) {
        const auto old_size = static_cast<std::uint32_t>(size());
        indices_.erase(entries_[i].hash, [i](std::uint32_t v) { return v == i; });
        std::pair<K, V> out{std::move(entries_[i].key), std::move(entries_[i].value)};
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        fix_indices_removed(static_cast<std::uint32_t>(i), old_size);
        return out;
    }

    // O(1) 弹出末尾元素。
    std::optional<std::pair<K, V>> pop() {
        if (empty()) return std::nullopt;
        return swap_remove_index(size() - 1);
    }

    // ---- 顺序调整 ----
    // 把下标 from 的元素移到 to（其余元素相对顺序不变）。O(n)。
    void move_index(size_type from, size_type to) {
        if (from == to) return;
        Bucket b = std::move(entries_[from]);
        if (from < to) {
            std::move(entries_.begin() + static_cast<std::ptrdiff_t>(from) + 1,
                      entries_.begin() + static_cast<std::ptrdiff_t>(to) + 1,
                      entries_.begin() + static_cast<std::ptrdiff_t>(from));
            for (auto j = static_cast<std::uint32_t>(from); j < to; ++j) {
                // 搬移后 entries_[j] 的旧下标是 j+1
                indices_.update_value(entries_[j].hash, j,
                                      [j1 = j + 1](std::uint32_t v) { return v == j1; });
            }
        } else {
            std::move_backward(entries_.begin() + static_cast<std::ptrdiff_t>(to),
                                entries_.begin() + static_cast<std::ptrdiff_t>(from),
                                entries_.begin() + static_cast<std::ptrdiff_t>(from) + 1);
            for (auto j = static_cast<std::uint32_t>(from); j > to; --j) {
                // 搬移后 entries_[j-1] 的旧下标是 j-1（右移一格，值不变？不：
                // 区间 [to, from) 右移，entries_[j] 旧下标 j-1）
                indices_.update_value(entries_[j].hash, j,
                                      [j0 = j - 1](std::uint32_t v) { return v == j0; });
            }
        }
        indices_.update_value(b.hash, static_cast<std::uint32_t>(to),
                              [f = static_cast<std::uint32_t>(from)](std::uint32_t v) { return v == f; });
        entries_[to] = std::move(b);
    }

    // 交换两个下标的元素。O(1)。
    void swap_indices(size_type a, size_type b) {
        if (a == b) return;
        const std::uint64_t ha = entries_[a].hash, hb = entries_[b].hash;
        const auto av = static_cast<std::uint32_t>(a), bv = static_cast<std::uint32_t>(b);
        const auto ea = [av](std::uint32_t v) { return v == av; };
        const auto eb = [bv](std::uint32_t v) { return v == bv; };
        if (ha == hb) {
            // 同 hash 时两次 update 会互相污染：先用哨兵占位
            indices_.update_value(ha, kInvalidIndex, ea);
            indices_.update_value(hb, av, eb);
            indices_.update_value(ha, bv, [](std::uint32_t v) { return v == kInvalidIndex; });
        } else {
            indices_.update_value(ha, bv, ea);  // a → b
            indices_.update_value(hb, av, eb);  // b → a
        }
        std::swap(entries_[a], entries_[b]);
    }

    // ---- 排序 / 过滤 ----
    // 就地按 key 升序（用缓存 hash 重建索引表，不重算哈希）。排序后新插入仍在末尾追加。
    void sort_keys() {
        std::sort(entries_.begin(), entries_.end(),
                  [](const Bucket& x, const Bucket& y) { return x.key < y.key; });
        rebuild_indices();
    }
    template <class Cmp>
    void sort_by(Cmp cmp) {
        std::sort(entries_.begin(), entries_.end(),
                  [&cmp](const Bucket& x, const Bucket& y) { return cmp(x.key, x.value, y.key, y.value); });
        rebuild_indices();
    }

    // 条件保留（pred(key, value) 为真留下）。返回删除元素个数。
    // 与文章一致：只在确有删除时才重建索引表。
    template <class Pred>
    size_type retain(Pred pred) {
        const std::size_t n = size();
        std::size_t w = 0;
        for (std::size_t r = 0; r < n; ++r) {
            if (pred(entries_[r].key, entries_[r].value)) {
                if (w != r) entries_[w] = std::move(entries_[r]);
                ++w;
            }
        }
        const std::size_t removed = n - w;
        if (removed) {
            entries_.resize(w);
            rebuild_indices();
        }
        return removed;
    }

    // ---- 迭代（插入序）----
    iterator begin() { return iterator(entries_.begin()); }
    iterator end() { return iterator(entries_.end()); }
    const_iterator begin() const { return const_iterator(entries_.begin()); }
    const_iterator end() const { return const_iterator(entries_.end()); }

    // ---- 诊断（基准报告用）----
    std::size_t index_table_memory_bytes() const { return indices_.memory_bytes(); }
    std::size_t entries_memory_bytes() const { return entries_.capacity() * sizeof(Bucket); }
    std::size_t index_table_buckets() const { return indices_.buckets(); }

private:
    std::vector<Bucket> entries_;
    SwissIndexTable indices_;
    Hash hash_{};
    KeyEq eq_{};

    // 通用查找（含空表/单元素捷径——文章 3.1：单元素连哈希都不算）
    template <class MapRef, class Q, class QHash, class QEq>
    static auto find_ptr(MapRef& self, const Q& q, QHash qh, QEq qe) {
        using VPtr = std::conditional_t<std::is_const_v<std::remove_reference_t<MapRef>>, const V*, V*>;
        const std::size_t idx = self.index_of(q, qh, qe);
        return VPtr(idx == npos ? nullptr : &self.entries_[idx].value);
    }

    template <class Q, class QHash, class QEq>
    std::size_t index_of(const Q& q, QHash qh, QEq qe) const {
        if (entries_.empty()) return npos;
        if (entries_.size() == 1) return qe(q, entries_[0].key) ? 0 : npos;
        const std::uint64_t h = qh(q);
        return indices_.find(h, [&](std::uint32_t v) { return qe(q, entries_[v].key); });
    }

    // 已算好哈希的查重（insert 热路径）
    std::size_t find_index_by_hash(std::uint64_t h, const K& k) const {
        if (entries_.size() == 1) return eq_(k, entries_[0].key) ? 0 : npos;
        return indices_.find(h, [&](std::uint32_t v) { return eq_(k, entries_[v].key); });
    }

    void push_new(std::uint64_t h, K k, V v) {
        if (IM_UNLIKELY(entries_.size() == entries_.capacity()))
            entries_.reserve(indices_.capacity());  // 与索引表容量对齐
        const auto idx = static_cast<std::uint32_t>(size());
        indices_.insert_new(h, idx, [this](std::uint32_t j) { return entries_[j].hash; });
        entries_.push_back(Bucket{h, std::move(k), std::move(v)});
    }

    // shift_remove 之后：区间 (begin, old_end) 的下标全部 -1（启发式选路径，文章 3.4）
    void fix_indices_removed(std::uint32_t begin, std::uint32_t old_end) {
        const std::size_t shifted = old_end - begin - 1;
        if (shifted > indices_.capacity() / 2) {
            indices_.decrement_range(begin + 1, old_end);
        } else {
            for (auto j = begin; j + 1 < old_end; ++j) {
                // entries_[j] 搬移后的旧下标是 j+1
                indices_.update_value(entries_[j].hash, j,
                                      [j1 = j + 1](std::uint32_t v) { return v == j1; });
            }
        }
    }

    // shift_insert 之后：区间 [begin+1, new_end) 的下标全部 +1
    void fix_indices_inserted(std::uint32_t begin, std::uint32_t old_end) {
        const std::size_t shifted = old_end - begin;
        if (shifted > indices_.capacity() / 2) {
            indices_.increment_range(begin + 1, old_end + 1);
        } else {
            // 从大到小改写，避免把刚写入的新值再次匹配
            for (auto j = old_end; j > begin; --j) {
                indices_.update_value(entries_[j].hash, j,
                                      [j0 = j - 1](std::uint32_t v) { return v == j0; });
            }
        }
    }

    // 排序/retain 后：用缓存 hash 全量重建索引表
    void rebuild_indices() {
        indices_.clear();
        const auto n = static_cast<std::uint32_t>(size());
        for (std::uint32_t i = 0; i < n; ++i) {
            indices_.insert_new(entries_[i].hash, i,
                                [this](std::uint32_t j) { return entries_[j].hash; });
        }
    }
};

}  // namespace im
