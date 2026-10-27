# indexmap-cpp 构建脚本
# 用法：
#   make -j$(nproc) test        # 编译并运行单元测试（-O2）
#   make test_asan              # AddressSanitizer + UBSan 版测试
#   make -j$(nproc) bench       # 编译并运行性能对比（结果写入 output/）
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -march=native
BUILD    := build

.PHONY: test test_asan bench all clean

all: test

$(BUILD):
	mkdir -p $(BUILD)

# ---- 单元测试 ----
$(BUILD)/test_index_map: tests/test_index_map.cpp src/index_map.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_index_map.cpp -o $@

test: $(BUILD)/test_index_map
	./$(BUILD)/test_index_map

# 消毒剂版本（内存安全验证：手写内存管理的 SwissTable 必跑）
$(BUILD)/test_index_map_asan: tests/test_index_map.cpp src/index_map.hpp | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
		tests/test_index_map.cpp -o $@

test_asan: $(BUILD)/test_index_map_asan
	./$(BUILD)/test_index_map_asan

# ---- 性能对比 ----
$(BUILD)/bench_index_map: benchmarks/bench_index_map.cpp src/index_map.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) benchmarks/bench_index_map.cpp -o $@

bench: $(BUILD)/bench_index_map
	mkdir -p output
	./$(BUILD)/bench_index_map | tee output/benchmark-result.txt

clean:
	rm -rf $(BUILD)
