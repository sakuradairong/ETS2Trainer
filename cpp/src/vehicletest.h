// vehicletest.h —— 防侧翻备份管理的离线测试（自身进程内存，不依赖真实游戏）
//
// 覆盖：A→B / A→B→A 各自恢复独立原始重心、旧对象失效、第一次恢复失败第二次成功、
//       不同原始重心互不串扰、未恢复的记录不得被覆盖、cvar 逐项验证清除。
#pragma once

#include "enginetest.h"  // 复用通用测试条目 TunerTestItem

#include <vector>

namespace ets2 {

std::vector<TunerTestItem> runVehicleAntiRollTests();

}  // namespace ets2
