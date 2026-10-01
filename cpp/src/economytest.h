// economytest.h —— 经济定位候选筛选的离线测试（不依赖真实游戏）
//
// 覆盖：零候选 / 唯一候选 / 多个同分候选（歧义必须失败关闭）、
//       经验字段地址换算（直接使用扫描地址，不再重复加偏移）、
//       错误偏移拒绝、经济对象必须引用已定位的 bank 对象。
#pragma once

#include "enginetest.h"  // 复用通用测试条目 TunerTestItem

#include <vector>

namespace ets2 {

std::vector<TunerTestItem> runEconomyLocateTests();

}  // namespace ets2
