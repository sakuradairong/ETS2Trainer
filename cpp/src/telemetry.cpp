// telemetry.cpp —— RenCloud/truckermudgeon SCS telemetry shared-memory revision 12
#include "telemetry.h"

#include <cmath>
#include <cstring>

namespace ets2 {

namespace {

constexpr wchar_t kMappingName[] = L"Local\\SCSTelemetry";
constexpr size_t kMappingSize = 32 * 1024;

template <typename T>
T loadAt(const uint8_t* data, size_t offset) {
    T value{};
    ::memcpy(&value, data + offset, sizeof(value));
    return value;
}

bool plausible(const TelemetrySnapshot& s, std::string* error) {
    if (!s.sdkActive) {
        if (error) *error = "遥测插件已加载，但 SDK 当前未激活（请进入驾驶界面）";
        return false;
    }
    if (s.pluginRevision < 12) {
        if (error) *error = fmt("遥测插件版本过旧（revision %u，需要 12+）", s.pluginRevision);
        return false;
    }
    if (s.game != 1) {
        if (error) *error = fmt("共享内存不是欧卡2数据（game=%u）", s.game);
        return false;
    }
    if (!std::isfinite(s.fuel) || !std::isfinite(s.fuelCapacity) || s.fuel < 0.0f ||
        s.fuelCapacity <= 0.0f || s.fuelCapacity > 10000.0f ||
        s.fuel > s.fuelCapacity + 1000.0f) {
        if (error) *error = "遥测油量数据不合理，可能是不兼容的共享内存布局";
        return false;
    }
    for (float value : s.wear) {
        if (!std::isfinite(value) || value < -0.001f || value > 1.001f) {
            if (error) *error = "遥测损伤数据不合理，可能是不兼容的共享内存布局";
            return false;
        }
    }
    return true;
}

}  // namespace

bool decodeTelemetryBuffer(const void* raw, size_t size, TelemetrySnapshot* out,
                           std::string* error) {
    if (!raw || !out || size < 1056) {
        if (error) *error = "遥测共享内存数据不完整";
        return false;
    }
    const auto* data = static_cast<const uint8_t*>(raw);
    TelemetrySnapshot s;
    s.sdkActive = loadAt<uint8_t>(data, 0) != 0;
    s.paused = loadAt<uint8_t>(data, 4) != 0;
    s.timestamp = loadAt<uint64_t>(data, 8);
    s.pluginRevision = loadAt<uint32_t>(data, 40);
    s.gameMajor = loadAt<uint32_t>(data, 44);
    s.gameMinor = loadAt<uint32_t>(data, 48);
    s.game = loadAt<uint32_t>(data, 52);
    s.scale = loadAt<float>(data, 700);
    s.fuelCapacity = loadAt<float>(data, 704);
    s.speed = loadAt<float>(data, 948);
    s.fuel = loadAt<float>(data, 1000);
    for (size_t i = 0; i < s.wear.size(); ++i) {
        s.wear[i] = loadAt<float>(data, 1036 + i * sizeof(float));
    }
    if (!plausible(s, error)) return false;
    *out = s;
    return true;
}

bool readTelemetry(TelemetrySnapshot* out, std::string* error) {
    if (!out) {
        if (error) *error = "遥测输出参数为空";
        return false;
    }
    HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
    if (!mapping) {
        if (error) {
            *error = fmt("未找到 %s（请确认游戏已加载 scs-telemetry.dll，错误码 %lu）",
                         "Local\\SCSTelemetry", (unsigned long)::GetLastError());
        }
        return false;
    }
    const auto* view = static_cast<const uint8_t*>(
        ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, kMappingSize));
    if (!view) {
        DWORD code = ::GetLastError();
        ::CloseHandle(mapping);
        if (error) *error = fmt("打开遥测共享内存失败（错误码 %lu）", (unsigned long)code);
        return false;
    }

    bool ok = false;
    std::string localError;
    // 插件会持续更新该内存；用时间戳夹住复制，避免混合两个遥测帧。
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
        uint64_t before = loadAt<uint64_t>(view, 8);
        ::MemoryBarrier();
        uint8_t copy[1056];
        ::memcpy(copy, view, sizeof(copy));
        ::MemoryBarrier();
        uint64_t after = loadAt<uint64_t>(view, 8);
        if (before != after) {
            ::Sleep(1);
            continue;
        }
        ok = decodeTelemetryBuffer(copy, sizeof(copy), out, &localError);
    }
    ::UnmapViewOfFile(view);
    ::CloseHandle(mapping);
    if (!ok && error) {
        *error = localError.empty() ? "遥测数据更新过快，请重试" : localError;
    }
    return ok;
}

}  // namespace ets2
