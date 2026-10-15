// saveprofilestest.h —— 无真实文件：存档按档案分组 / 选择恢复的合成回归
#pragma once
#include "enginetest.h"

namespace ets2 {
// 使用内存构造的 SaveSlot 列表驱动分组与选择恢复，不读写任何真实存档。
std::vector<TunerTestItem> runSaveProfileTests();

}  // namespace ets2
