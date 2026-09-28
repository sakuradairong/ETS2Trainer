// enginetest.h —— 不依赖真实游戏的 EngineTuner 状态机 / 并发测试
#pragma once

#include <string>
#include <vector>

namespace ets2 {

struct TunerTestItem {
    std::string name;
    bool        ok = false;
    std::string detail;
};

// 使用内存镜像 + 脚本化定位器驱动 EngineTuner，无需游戏进程。
std::vector<TunerTestItem> runEngineTunerTests();

}  // namespace ets2
