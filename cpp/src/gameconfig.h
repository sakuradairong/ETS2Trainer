// gameconfig.h —— 游戏 config.cfg 的读取与行保留改写（飞行模式等开关用）
#pragma once

#include "common.h"

#include <string>

namespace ets2 {

// Documents\Euro Truck Simulator 2\config.cfg
std::wstring gameConfigPath();

// 从 config.cfg 文本读取 uset 键的值（去掉引号）；找不到返回 false。
bool readConfigKey(const std::string& text, const std::string& key, std::string* value);

// 行保留地改写 uset 键：只动目标那一行，其它字节（含换行风格）保持不变；
// 键不存在时追加到末尾。changed 表示是否真的改动了内容。
bool rewriteConfigKey(std::string* text, const std::string& key, const std::string& value,
                      bool* changed);

// 读取整个 config.cfg。
bool readGameConfigText(std::string* text, std::string* error);

// 写回 config.cfg；首次写入前把原文件备份为 config.cfg.trainer.bak。
bool writeGameConfigText(const std::string& text, std::string* error,
                         std::wstring* backupPath);

}  // namespace ets2
