// telemetry.h —— 读取现有 SCS 遥测插件的只读共享内存
#pragma once

#include "common.h"

#include <array>
#include <string>

namespace ets2 {

struct TelemetrySnapshot {
    bool sdkActive = false;
    bool paused = false;
    uint64_t timestamp = 0;
    uint32_t pluginRevision = 0;
    uint32_t gameMajor = 0;
    uint32_t gameMinor = 0;
    uint32_t game = 0;  // 1=ETS2, 2=ATS
    float scale = 0.0f;
    float fuelCapacity = 0.0f;
    float speed = 0.0f;
    float fuel = 0.0f;
    std::array<float, 5> wear{};  // engine, transmission, cabin, chassis, wheels
};

// 供自检使用，也把共享内存布局校验集中在一个地方。
bool decodeTelemetryBuffer(const void* data, size_t size, TelemetrySnapshot* out,
                           std::string* error);

// 打开 Local\SCSTelemetry 并读取一个一致快照。该函数不向游戏或共享内存写入。
bool readTelemetry(TelemetrySnapshot* out, std::string* error);

}  // namespace ets2
