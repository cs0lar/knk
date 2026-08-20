#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "kernel/storage_engine.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

// 辅助函数：创建独立的临时测试目录
std::filesystem::path test_root(const std::string &name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

// 辅助函数：生成一条模拟断言数据
Assertion make_assertion(AssertionId id, EntityId subject) {
    return Assertion{.id = id,
                     .subject = subject,
                     .predicate = 10,
                     .object = 100,
                     .valid_from = 1672531200,
                     .valid_to = OPEN_ENDED,
                     .observed_at = 1719878400,
                     .confidence = 0.95,
                     .status = AssertionStatus::Active};
}

// 测试 1：测试基本的追加写入与读取
void storage_engine_appends_and_reads_assertions() {
    auto dir = test_root("kernel_se_appends_and_reads");
    
    // 使用 C++20 指派初始化器，与代码库风格保持一致
    StorageConfig config{.root = dir, .max_records_per_segment = 100};
    StorageEngine engine(config);

    engine.append_assertion(make_assertion(1, 1));
    auto assertions = engine.load_assertions();

    assert(assertions.size() == 1);
    assert(assertions[0].id == 1);
    assert(assertions[0].subject == 1);

    std::filesystem::remove_all(dir);
}

// 测试 2：测试模拟奔溃/重启后的数据重放恢复 (Recovery via Replay)
void storage_engine_recovers_data_on_reopen() {
    auto dir = test_root("kernel_se_recovers_data");
    StorageConfig config{.root = dir, .max_records_per_segment = 100};

    {
        // 第一次打开：写入两条数据
        StorageEngine engine(config);
        engine.append_assertion(make_assertion(1, 1));
        engine.append_assertion(make_assertion(2, 2));
    } // 离开作用域，engine 销毁，storage_lock_ 释放

    {
        // 第二次打开：模拟基于已存在数据的目录重启
        StorageEngine engine_reopened(config);
        auto assertions = engine_reopened.load_assertions();
        
        // 验证数据已成功恢复
        assert(assertions.size() == 2);
        assert(assertions[0].id == 1);
        assert(assertions[1].id == 2);
    }

    std::filesystem::remove_all(dir);
}

// 测试 3：测试它对下游日志（例如 AssertionLog）归档功能的协调委托
void storage_engine_delegates_archiving_and_hints() {
    auto dir = test_root("kernel_se_delegates_archiving");
    StorageConfig config{.root = dir, .max_records_per_segment = 2}; // 容量设为2，强制分段
    
    StorageEngine engine(config);
    engine.append_assertion(make_assertion(1, 1));
    engine.append_assertion(make_assertion(2, 2));
    engine.append_assertion(make_assertion(3, 3));

    // 验证行数提示的传递
    assert(engine.assertion_log_record_count_hint() == 3);

    // 验证归档功能的委派
    engine.archive_segments_before(3);
    auto assertions = engine.load_assertions_after(2);
    
    assert(assertions.size() == 1);
    assert(assertions[0].id == 3);

    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    storage_engine_appends_and_reads_assertions();
    storage_engine_recovers_data_on_reopen();
    storage_engine_delegates_archiving_and_hints();

    std::cout << "All storage_engine tests passed.\n";
    return 0;
}