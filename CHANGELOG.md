# 更新日志

## 1.0.0（2026-10-27）

首个版本。

### 新增

- `src/index_map.hpp`：高性能保插入序哈希映射（Rust indexmap 的 C++ 移植，单头文件）
  - entries 稠密数组（Bucket 缓存哈希）+ 自研 SwissTable 索引表（SSE2 组探测、
    三角探测、7/8 负载、墓碑删除 + rehash_in_place/倍增；非 x86 标量回退）
  - API：insert/insert_full/find/find_equiv（异构）/get_index/get_index_of/
    swap_remove/shift_remove/shift_insert/move_index/swap_indices/sort_keys/
    sort_by/retain/first/last/pop/reserve/clear
  - 低延迟优化：探测组单次加载、ctrl+slots 单块分配、ctrl_is_full 单比较、
    likely/unlikely 分支提示
- `tests/`：15 用例 / 56,344 断言单元测试（含随机模型对照、全碰撞哈希器、
  ASan/UBSan 验证）
- `benchmarks/`：vs std::unordered_map / std::map 三规模性能对比
- `docs/knowledge-points.md`：来源文章知识点完整提炼（设计依据）
- `output/benchmark-result.txt`：基准原始输出
