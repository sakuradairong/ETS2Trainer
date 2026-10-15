// saveprofilesuitest.h —— 合成 3 组槽位的存档按档案分组 UI 冒烟探针
#pragma once
#include "ui.h"

#include <string>

namespace ets2 {
// 在 UI 冒烟帧中调用：装配三组合成槽位、驱动生产 selectSaveProfile 切换、
// 注入鼠标点击，记录可见槽位数量与被清空的旧槽/数值。
void saveProfilesSmokeRequest(AppState& app, int frame);
void saveProfilesSmokeReport(AppState& app, int& smokeResult);

}  // namespace ets2
