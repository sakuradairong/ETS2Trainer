// engine.cpp —— 有界只读的附属总成探针实现 + 发动机动力调节
#include "engine.h"

#include "gameplay.h"
#include "layout.h"

#include "vehicle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace ets2 {

namespace {

// 硬上限：任何一次探针都不会超过这些额度（不遍历堆、不做数值扫描）。
constexpr uint64_t kRootWindow = 0x2000;   // 根对象（车辆核心对象）读取窗口
constexpr uint64_t kChildWindow = 0x800;   // 子对象读取窗口
constexpr uint64_t kStringWindow = 0x100;  // 每个节点用于寻找名称字符串的前缀窗口
constexpr uint64_t kParentWindow = 0x1000;
constexpr size_t kMaxNodes = 8192;
constexpr int kMaxDepth = 4;
constexpr uint64_t kMaxBytes = 24ull * 1024 * 1024;
constexpr uint64_t kDumpBegin = 0x180;
constexpr uint64_t kDumpEnd = 0x2A0;
constexpr size_t kMaxNamed = 40;

struct Node {
    uint64_t address = 0;
    int depth = 0;
    int parent = -1;
    uint64_t offset = 0;  // 在父对象中的偏移
    std::string label;    // 根节点名称（非根为空）
};

bool isCanonical(uint64_t value) {
    return value >= 0x10000ull && value <= 0x00007FFFFFFFFFFFull;
}

bool readFloat(const ProcessMemory& memory, uint64_t address, float* value) {
    float local = 0.0f;
    if (!memory.read(address, &local, sizeof(local)) || !std::isfinite(local)) return false;
    *value = local;
    return true;
}

bool inRange(float value, float low, float high) {
    return std::isfinite(value) && value >= low && value <= high;
}

std::string lowerCase(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return text;
}

// 读取一个指针指向的可打印 ASCII 字符串（最多 63 字节）。
bool readAsciiString(const ProcessMemory& memory, uint64_t address, size_t maxLength,
                     std::string* out) {
    char text[64] = {};
    size_t got = 0;
    if (maxLength >= sizeof(text)) maxLength = sizeof(text) - 1;
    if (!memory.read(address, text, maxLength, &got) || got < 2) return false;
    std::string value;
    for (size_t i = 0; i < got; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c == 0) break;
        if (c < 32 || c > 126) { value.clear(); break; }
        value.push_back((char)c);
    }
    if (value.size() < 3) return false;
    *out = value;
    return true;
}

std::string buildPath(const std::vector<Node>& nodes, size_t index) {
    std::vector<std::string> parts;
    int current = (int)index;
    while (current >= 0) {
        const Node& node = nodes[(size_t)current];
        if (node.parent < 0) {
            parts.push_back(node.label.empty() ? std::string("root") : node.label);
        } else {
            char buffer[32];
            ::snprintf(buffer, sizeof(buffer), "+0x%llX", (unsigned long long)node.offset);
            parts.push_back(buffer);
        }
        current = node.parent;
    }
    std::reverse(parts.begin(), parts.end());
    std::string text;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) text += " -> ";
        text += parts[i];
    }
    return text;
}

void dumpFloatsAndPointers(const ProcessMemory& memory, uint64_t base, uint64_t begin, uint64_t end,
                           std::vector<std::pair<uint64_t, float>>* floats,
                           std::vector<std::pair<uint64_t, uint64_t>>* pointers) {
    std::vector<uint8_t> window((size_t)(end - begin), 0);
    size_t got = 0;
    if (!memory.read(base + begin, window.data(), window.size(), &got) || got < 8) return;
    if (floats) {
        for (size_t offset = 0; offset + 4 <= got; offset += 4) {
            float value = 0.0f;
            ::memcpy(&value, window.data() + offset, sizeof(value));
            if (std::isfinite(value) && std::fabs(value) > 1e-6f && std::fabs(value) < 1e7f) {
                floats->emplace_back(begin + offset, value);
            }
        }
    }
    if (pointers) {
        for (size_t offset = 0; offset + 8 <= got; offset += 8) {
            uint64_t pointer = 0;
            ::memcpy(&pointer, window.data() + offset, sizeof(pointer));
            if (!isCanonical(pointer)) continue;
            pointers->emplace_back(begin + offset, pointer);
        }
    }
}

// 在已读入的窗口里找出指向名称字符串的指针（用 probed 缓存避免重复读取同一地址）。
void scanWindowStrings(const ProcessMemory& memory, const std::vector<uint8_t>& window, size_t got,
                       uint64_t limit, std::unordered_set<uint64_t>* probed,
                       std::vector<std::pair<uint64_t, std::string>>* out) {
    const size_t capped = (size_t)(std::min<uint64_t>((uint64_t)got, limit));
    for (size_t offset = 0; offset + 8 <= capped; offset += 8) {
        uint64_t pointer = 0;
        ::memcpy(&pointer, window.data() + offset, sizeof(pointer));
        if (!isCanonical(pointer)) continue;
        if (probed) {
            if (probed->count(pointer)) continue;
            probed->insert(pointer);
        }
        std::string text;
        if (!readAsciiString(memory, pointer, 63, &text)) continue;
        out->emplace_back(offset, text);
    }
}

bool looksLikeEngineData(const ProcessMemory& memory, uint64_t address, AccessoryHit* hit) {
    float torque = 0.0f, power = 0.0f, idle = 0.0f, limit = 0.0f, low = 0.0f, high = 0.0f;
    if (!readFloat(memory, address + engine_field::kTorque, &torque) ||
        !readFloat(memory, address + engine_field::kRpmIdle, &idle) ||
        !readFloat(memory, address + engine_field::kRpmLimit, &limit) ||
        !readFloat(memory, address + engine_field::kRpmRangeLowGear, &low) ||
        !readFloat(memory, address + engine_field::kRpmRangeHighGear, &high)) {
        return false;
    }
    if (!inRange(torque, 200.0f, 20000.0f)) return false;
    if (!inRange(limit, 400.0f, 8000.0f)) return false;
    if (!inRange(idle, 200.0f, 1500.0f) || idle >= limit) return false;
    if (!inRange(low, 300.0f, 3000.0f) || !inRange(high, 300.0f, 4000.0f)) return false;
    if (low > high) return false;
    readFloat(memory, address + engine_field::kPower, &power);
    hit->torque = torque;
    hit->power = power;
    hit->rpmIdle = idle;
    hit->rpmLimit = limit;
    hit->rpmRangeLow = low;
    hit->rpmRangeHigh = high;
    return true;
}

// 记录父容器里同一批数据对象的指针槽，便于找出与变速箱数据同族的发动机数据。
void collectFamily(const ProcessMemory& memory, uint64_t parent, std::vector<FamilyEntry>* out) {
    std::vector<uint8_t> window((size_t)kParentWindow, 0);
    size_t got = 0;
    if (!memory.read(parent, window.data(), window.size(), &got) || got < 16) return;
    for (size_t offset = 0; offset + 8 <= got; offset += 8) {
        uint64_t pointer = 0;
        ::memcpy(&pointer, window.data() + offset, sizeof(pointer));
        if (!isCanonical(pointer)) continue;
        FamilyEntry entry;
        entry.slot = parent + offset;
        entry.target = pointer;
        for (uint64_t probe = 0x8; probe <= 0x60; probe += 0x20) {
            std::string text;
            if (readAsciiString(memory, pointer + probe, 63, &text)) {
                entry.name = text;
                break;
            }
        }
        std::vector<std::pair<uint64_t, float>> nums;
        dumpFloatsAndPointers(memory, pointer, 0x1B0, 0x270, &nums, nullptr);
        entry.nums = std::move(nums);
        out->push_back(std::move(entry));
        if (out->size() >= 64) break;
    }
}

bool looksLikeTransmissionData(const ProcessMemory& memory, uint64_t address, AccessoryHit* hit) {
    float stall = 0.0f, differential = 0.0f;
    if (!readFloat(memory, address + transmission_field::kStallTorqueRatio, &stall) ||
        !readFloat(memory, address + transmission_field::kDifferentialRatio, &differential)) {
        return false;
    }
    if (!inRange(stall, 0.8f, 6.0f) || !inRange(differential, 0.8f, 12.0f)) return false;
    hit->stallTorqueRatio = stall;
    hit->differentialRatio = differential;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 发动机数据对象定位（只读）
// ---------------------------------------------------------------------------
namespace {

bool readEngineSignature(const ProcessMemory& memory, uint64_t address, float* torque,
                         float* rpmIdle, float* rpmLimit, float* rpmLimitNeutral,
                         float* resistance) {
    if (!readFloat(memory, address + engine_runtime::kTorque, torque)) return false;
    if (!readFloat(memory, address + engine_runtime::kRpmIdle, rpmIdle)) return false;
    if (!readFloat(memory, address + engine_runtime::kRpmLimit, rpmLimit)) return false;
    if (!readFloat(memory, address + engine_runtime::kRpmLimitNeutral, rpmLimitNeutral)) return false;
    readFloat(memory, address + engine_runtime::kResistanceTorque, resistance);
    if (!inRange(*torque, 100.0f, 60000.0f)) return false;  // 允许已被本程序放大的扭矩
    if (!inRange(*rpmIdle, 300.0f, 1200.0f)) return false;
    if (!inRange(*rpmLimit, 1200.0f, (selectedGame() == GameId::Ats ? 10000.0f : 4500.0f))) return false;
    if (!inRange(*rpmLimitNeutral, 1200.0f, (selectedGame() == GameId::Ats ? 10000.0f : 4500.0f))) return false;
    if (*rpmIdle >= *rpmLimit) return false;
    if (*rpmLimitNeutral > *rpmLimit * 1.5f) return false;
    if (*resistance < 0.0f || *resistance > 3000.0f) return false;
    return true;
}

bool readTransmissionSignature(const ProcessMemory& memory, uint64_t address, float* differential,
                               float* stall) {
    if (!readFloat(memory, address + transmission_runtime::kDifferentialRatio, differential) ||
        !readFloat(memory, address + transmission_runtime::kStallTorqueRatio, stall)) {
        return false;
    }
    return inRange(*differential, 1.5f, 9.0f) && inRange(*stall, 0.4f, 6.0f);
}

}  // namespace

bool locateEngineTarget(const ProcessMemory& memory, uint64_t context, EngineTarget* target,
                        std::string* error) {
    if (!memory.isOpen()) {
        if (error) *error = "游戏进程尚未附加";
        return false;
    }
    if (!isCanonical(context)) {
        if (error) *error = "玩家上下文无效";
        return false;
    }
    uint64_t imageBase = 0;
    if (selectedGame() == GameId::Ats && !verifyGameLayout(memory, &imageBase, error)) return false;
    uint64_t container = 0;
    if (!memory.read(context + kContextToAccessoryData, &container, sizeof(container)) ||
        !isCanonical(container)) {
        if (error) *error = "读取附属总成数据容器失败";
        return false;
    }
    std::vector<uint8_t> window((size_t)kParentWindow, 0);
    size_t got = 0;
    if (!memory.read(container, window.data(), window.size(), &got) || got < 16) {
        if (error) *error = "读取附属总成数据容器内容失败";
        return false;
    }

    EngineTarget found;
    found.container = container;
    std::vector<uint64_t> engineMatches;
    for (size_t offset = 0; offset + 8 <= got; offset += 8) {
        uint64_t pointer = 0;
        ::memcpy(&pointer, window.data() + offset, sizeof(pointer));
        if (!isCanonical(pointer)) continue;
        if (selectedGame() == GameId::Ats) {
            uint64_t vtable = 0;
            if (!memory.read(pointer, &vtable, sizeof(vtable)) ||
                vtable != imageBase + layoutProfile(GameId::Ats).engineVtableRva) continue;
        }
        float torque = 0.0f, idle = 0.0f, limit = 0.0f, neutral = 0.0f, resistance = 0.0f;
        if (readEngineSignature(memory, pointer, &torque, &idle, &limit, &neutral, &resistance)) {
            engineMatches.push_back(pointer);
            found.engineData = pointer;
            found.torque = torque;
            found.rpmIdle = idle;
            found.rpmLimit = limit;
            found.rpmLimitNeutral = neutral;
            found.resistanceTorque = resistance;
        } else {
            float differential = 0.0f, stall = 0.0f;
            if (found.transmissionData == 0 &&
                readTransmissionSignature(memory, pointer, &differential, &stall)) {
                found.transmissionData = pointer;
                found.differentialRatio = differential;
                found.stallTorqueRatio = stall;
            }
        }
    }
    found.engineMatches = (int)engineMatches.size();

    if (engineMatches.empty()) {
        if (error) {
            *error = fmt("容器内没有符合发动机签名的对象（请进入驾驶界面后重试）");
        }
        return false;
    }
    if (engineMatches.size() > 1) {
        if (error) {
            *error = fmt("容器内匹配到 %d 个发动机数据对象，已拒绝使用（无法唯一确定）",
                         (int)engineMatches.size());
        }
        return false;
    }

    dumpFloatsAndPointers(memory, found.engineData, engine_runtime::kLayoutBegin,
                          engine_runtime::kLayoutEnd, &found.layout, nullptr);
    readFloat(memory, found.engineData + engine_runtime::kTorqueCurveTail, &found.torqueCopyA);
    readFloat(memory, found.engineData + engine_runtime::kTorqueCurveTail + 4, &found.torqueCopyB);
    if (target) *target = std::move(found);
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------------------
// 内存适配 / 字段读写 / 生产定位器
// ---------------------------------------------------------------------------
namespace {

// 固定的 4 级指针链 + 玩家车辆遥测校验，只为拿到「玩家上下文」。
bool resolvePlayerContext(ProcessMemory& memory, DWORD pid, uint64_t* context,
                          std::string* error) {
    VehicleLocker locker;
    std::string bindError;
    if (!locker.bind(&memory, pid, &bindError)) {
        if (error) *error = "车辆版本指纹未通过：" + bindError;
        return false;
    }
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!locker.probe(&addresses, &telemetry, &bindError)) {
        if (error) *error = "无法定位当前车辆：" + bindError;
        return false;
    }
    *context = addresses.context;
    return true;
}

bool tunerClose(float a, float b, float tolerance) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tolerance;
}

// 写入安全范围：这里只是最后一道保险，正常路径另有「唯一对象 + 签名」约束。
constexpr float kMinScale = 0.9f;
constexpr float kMaxScale = 3.0f;
constexpr float kMinWriteTorque = 50.0f;
constexpr float kMaxWriteTorque = 25000.0f;
constexpr float kMinWriteRpm = 400.0f;
constexpr float kMaxWriteRpm = 6000.0f;
constexpr uint64_t kGuardIntervalMs = 5000;
constexpr uint64_t kMaintenanceIntervalMs = 1000;

// 扭矩副本只有在与原厂扭矩同量级时才一起缩放，避免误改相邻字段。
bool tunerSameMagnitude(float value, float reference) {
    if (!std::isfinite(value) || !std::isfinite(reference) || reference <= 0.0f) return false;
    return std::fabs(value - reference) <= reference * 0.25f;
}

bool tunerSameTriple(const EngineFieldValues& a, const EngineFieldValues& b) {
    // rpmLimitNeutral 必须参与比较：游戏可能分步写回各字段（扭矩/转速已回原厂、
    // 空挡转速还停在 +10% 的值），漏比会造成"看起来是原厂"而跳过恢复。
    return tunerClose(a.torque, b.torque, 0.5f) && tunerClose(a.copyA, b.copyA, 0.5f) &&
           tunerClose(a.copyB, b.copyB, 0.5f) && tunerClose(a.rpmLimit, b.rpmLimit, 0.5f) &&
           tunerClose(a.rpmLimitNeutral, b.rpmLimitNeutral, 0.5f);
}

bool tunerReadFloat(const EngineMemory& memory, uint64_t address, float* value) {
    float local = 0.0f;
    if (!memory.readBytes(address, &local, sizeof(local)) || !std::isfinite(local)) return false;
    *value = local;
    return true;
}

bool tunerWriteFloat(EngineMemory& memory, uint64_t address, float value) {
    return memory.writeBytes(address, &value, sizeof(value));
}

}  // namespace

bool ProcessMemoryAdapter::readBytes(uint64_t address, void* dst, size_t size) const {
    return memory_ != nullptr && memory_->read(address, dst, size);
}

bool ProcessMemoryAdapter::writeBytes(uint64_t address, const void* src, size_t size) {
    return memory_ != nullptr && memory_->write(address, src, size);
}

bool readEngineValues(const EngineMemory& memory, uint64_t object, EngineFieldValues* out) {
    EngineFieldValues values;
    if (!tunerReadFloat(memory, object + engine_runtime::kTorque, &values.torque)) return false;
    if (!tunerReadFloat(memory, object + engine_runtime::kRpmIdle, &values.rpmIdle)) return false;
    if (!tunerReadFloat(memory, object + engine_runtime::kRpmLimit, &values.rpmLimit)) return false;
    if (!tunerReadFloat(memory, object + engine_runtime::kRpmLimitNeutral,
                        &values.rpmLimitNeutral)) {
        return false;
    }
    if (!tunerReadFloat(memory, object + engine_runtime::kResistanceTorque, &values.resistance)) {
        return false;
    }
    if (!tunerReadFloat(memory, object + engine_runtime::kTorqueCurveTail, &values.copyA)) {
        return false;
    }
    if (!tunerReadFloat(memory, object + engine_runtime::kTorqueCurveTail + 4, &values.copyB)) {
        return false;
    }
    if (out) *out = values;
    return true;
}

bool engineValuesPlausible(const EngineFieldValues& values) {
    // 扭矩上限故意放宽：允许读到自己放大过的数值，否则放大后无法再定位与恢复。
    if (!std::isfinite(values.torque) || values.torque < 100.0f || values.torque > 60000.0f) {
        return false;
    }
    if (!std::isfinite(values.rpmIdle) || values.rpmIdle < 300.0f || values.rpmIdle > 1200.0f) {
        return false;
    }
    if (!std::isfinite(values.rpmLimit) || values.rpmLimit < 1200.0f ||
        values.rpmLimit > (selectedGame() == GameId::Ats ? 10000.0f : 4500.0f)) {
        return false;
    }
    if (!std::isfinite(values.rpmLimitNeutral) || values.rpmLimitNeutral < 1200.0f ||
        values.rpmLimitNeutral > (selectedGame() == GameId::Ats ? 10000.0f : 4500.0f)) {
        return false;
    }
    if (values.rpmIdle >= values.rpmLimit) return false;
    if (values.rpmLimitNeutral > values.rpmLimit * 1.5f) return false;
    if (!std::isfinite(values.resistance) || values.resistance < 0.0f ||
        values.resistance > 3000.0f) {
        return false;
    }
    return true;
}

bool GameEngineResolver::resolve(uint64_t* engineObject, std::string* error) {
    if (!memory_ || !memory_->isOpen()) {
        if (error) *error = "游戏进程尚未附加";
        return false;
    }
    uint64_t context = 0;
    if (!resolvePlayerContext(*memory_, pid_, &context, error)) return false;
    EngineTarget target;
    if (!locateEngineTarget(*memory_, context, &target, error)) return false;
    if (target.engineMatches != 1) {
        if (error) *error = fmt("发动机数据对象命中 %d 个，已拒绝写入", target.engineMatches);
        return false;
    }
    *engineObject = target.engineData;
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------------------
// 动力调节
// ---------------------------------------------------------------------------
EngineTuner::EngineTuner() {
    thread_ = std::thread([this]() { run(); });
}

EngineTuner::~EngineTuner() {
    restoreAndDetach();
    threadStop_.store(true);
    waitCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void EngineTuner::attach(std::unique_ptr<EngineMemory> memory,
                         std::unique_ptr<EngineTargetResolver> resolver, Guard guard) {
    std::lock_guard<std::mutex> lock(mutex_);
    memory_ = nullptr;
    memoryOwner_ = std::move(memory);
    memory_ = memoryOwner_.get();
    resolver_ = std::move(resolver);
    guard_ = std::move(guard);
    guardCheckedAt_ = 0;
    // 注意：不在这里清空 activeObject_ / objects_，否则「已经放大的旧对象」会失去恢复记录。
    setStatusLocked(active_ ? "已重新绑定游戏进程，动力调节继续保持"
                            : "已绑定游戏进程，动力调节待启用");
}

void EngineTuner::detach() {
    // 仅供进程句柄已不可用的情形：不写入，但保留尚未恢复的基准供持久化。
    std::lock_guard<std::mutex> lock(mutex_);
    const bool hadRecords = !objects_.empty() || activeObject_ != 0;
    active_ = false;
    activeObject_ = 0;
    for (auto it = objects_.begin(); it != objects_.end();) {
        if (it->second.hasWritten) ++it;
        else it = objects_.erase(it);
    }
    memory_ = nullptr;
    memoryOwner_.reset();
    resolver_.reset();
    guard_ = nullptr;
    if (hadRecords) bumpRevisionLocked();  // 待恢复记录集合可能已变化
    setStatusLocked(objects_.empty() ? "已分离：动力调节已关闭"
                                     : "已分离：原值未确认恢复，恢复记录仍保留");
}

void EngineTuner::restoreAndDetach() {
    std::lock_guard<std::mutex> lock(mutex_);
    // guard 命中/后台异常时 active_ 可能已经为 false，但未恢复的记录还存在；
    // 退出时必须继续尝试恢复，而不是只检查 active_。
    uint64_t target = activeObject_;
    if (!target) {
        for (const auto& [address, state] : objects_) {
            if (state.hasWritten) { target = address; break; }
        }
    }
    bool keepRecord = false;
    std::string detail;
    if (target) {
        if (!memory_ || !memory_->usable()) {
            keepRecord = true;
            detail = "游戏进程不可访问，原值未确认恢复";
        } else {
            const RestoreOutcome outcome = restoreObjectLocked(target, &detail);
            keepRecord = outcome == RestoreOutcome::Failed;
            if (outcome == RestoreOutcome::Ok) detail.clear();
        }
    }
    active_ = false;
    activeObject_ = 0;
    if (keepRecord) {
        // 只保留失败对象，供 exportState() 在分离后持久化。
        for (auto it = objects_.begin(); it != objects_.end();) {
            if (it->first == target) ++it;
            else it = objects_.erase(it);
        }
    } else objects_.clear();
    memory_ = nullptr;
    memoryOwner_.reset();
    resolver_.reset();
    guard_ = nullptr;
    bumpRevisionLocked();  // 恢复成败 / 待恢复记录集合发生变化
    setStatusLocked(keepRecord ? "原值未恢复，已保留恢复记录：" + detail
                               : "已停止动力调节" + (detail.empty() ? "" : "：" + detail));
}

bool EngineTuner::apply(float scale, bool raiseLimit, std::string* error) {
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        if (error) *error = reason;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!memory_ || !memory_->usable()) {
        if (error) *error = "尚未附加到游戏进程";
        return false;
    }
    if (!std::isfinite(scale) || scale < kMinScale || scale > kMaxScale) {
        if (error) *error = "动力倍率超出允许范围（0.9–3.0）";
        return false;
    }
    if (tunerClose(scale, 1.0f, 0.0001f) && !raiseLimit) {
        // 等价于“恢复原值”
        const bool hadActive = activeObject_ != 0;
        const bool ok = restoreActiveLocked(true, error);
        active_ = false;
        scale_ = 1.0f;
        raiseLimit_ = false;
        if (hadActive) bumpRevisionLocked();  // 恢复状态变化
        if (ok) setStatusLocked(summaryLocked());
        if (!hadActive && !remoteCreds_.empty()) {
            // 没有任何对象可恢复，却还存在未核对的导入记录 → 不得虚报成功
            if (error) {
                *error = fmt("上次运行的恢复记录尚未核对（共 %d 条），已保留",
                             (int)remoteCreds_.size());
            }
            setStatusLocked("动力调节已关闭；上次运行的恢复记录尚未核对，已保留");
            return false;
        }
        return ok;
    }
    scale_ = scale;
    raiseLimit_ = raiseLimit;
    std::string applyError;
    if (!applyLocked(scale, raiseLimit, &applyError)) {
        if (guardStopped_) {
            if (error) *error = applyError;
            return false;  // 保留守卫设置的真实恢复状态，勿用通用文案覆盖
        }
        active_ = false;
        // 失败时不留半途修改：把可能仍在生效的旧对象恢复原值。
        std::string restoreDetail;
        if (activeObject_) {
            restoreActiveLocked(false, &restoreDetail);
            if (!restoreDetail.empty()) applyError += "；旧对象回滚情况：" + restoreDetail;
        }
        if (error) *error = applyError;
        setStatusLocked("动力调节未生效：" + applyError);
        return false;
    }
    active_ = true;
    setStatusLocked(summaryLocked());
    if (error) error->clear();
    return true;
}

bool EngineTuner::release(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    scale_ = 1.0f;
    raiseLimit_ = false;
    if (!activeObject_) {
        for (const auto& [address, state] : objects_) {
            if (state.hasWritten) { activeObject_ = address; break; }
        }
    }
    if (!activeObject_) {
        if (!remoteCreds_.empty()) {
            // 还没有核对/恢复过任何对象：不能虚报「已恢复原厂」，记录必须保留，
            // 否则下一次启动会把上次的放大值当成原厂值再乘倍率。
            if (error) {
                *error = fmt("上次运行的恢复记录尚未核对（共 %d 条，未对任何对象写入或核对），已保留",
                             (int)remoteCreds_.size());
            }
            setStatusLocked("动力调节已关闭；上次运行的恢复记录尚未核对，已保留（不会把放大值当原厂值）");
            return false;
        }
        if (error) error->clear();
        setStatusLocked("动力调节已关闭");
        return true;
    }
    const bool ok = restoreActiveLocked(true, error);
    bumpRevisionLocked();  // 恢复成功或失败：待恢复记录状态变化
    setStatusLocked(ok ? "已恢复原厂动力" : ("已关闭动力调节（" + (error ? *error : std::string()) + "）"));
    return ok;
}

bool EngineTuner::maintenance(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return maintenanceLocked(error);
}

bool EngineTuner::maintenanceLocked(std::string* error) {
    if (!active_) {
        if (error) error->clear();
        return true;
    }
    if (!memory_ || !memory_->usable()) {
        active_ = false;
        activeObject_ = 0;
        setStatusLocked("动力调节已停止：游戏进程已关闭");
        if (error) *error = "游戏进程已关闭";
        return false;
    }
    std::string applyError;
    if (!applyLocked(scale_, raiseLimit_, &applyError)) {
        // 联机闸门自行设置状态并关闭 active_；不要把还原失败信息覆盖为泛化暂停。
        if (active_) setStatusLocked("动力调节已暂停：" + applyError);
        if (error) *error = applyError;
        return false;
    }
    setStatusLocked(summaryLocked());
    if (error) error->clear();
    return true;
}

bool EngineTuner::applyLocked(float scale, bool raiseLimit, std::string* error) {
    guardStopped_ = false;
    if (!memory_ || !memory_->usable()) {
        if (error) *error = "尚未附加到游戏进程";
        return false;
    }

    // 守卫与本次写入必须处于同一绑定的互斥区内；回调不得回查 tuner。
    const uint64_t now = ::GetTickCount64();
    if (guard_ && (activeObject_ == 0 || now - guardCheckedAt_ >= guardIntervalMs_)) {
        guardCheckedAt_ = now;
        std::string reason;
        if (!guard_(&reason)) {
            guardStopped_ = true;
            std::string detail;
            RestoreOutcome outcome = RestoreOutcome::Ok;
            const bool hadActive = activeObject_ != 0;
            if (activeObject_) outcome = restoreObjectLocked(activeObject_, &detail);
            active_ = false;
            activeObject_ = 0;
            if (hadActive) bumpRevisionLocked();  // 活动对象被移除 / 恢复状态变化
            if (outcome == RestoreOutcome::Failed) {
                setStatusLocked("联机保护已停止动力调节；原值未能恢复，恢复记录已保留：" + detail);
            } else if (outcome == RestoreOutcome::NotOurs) {
                setStatusLocked("联机保护已停止动力调节；对象已改变，未执行恢复：" + detail);
            } else {
                setStatusLocked("已自动停止并恢复原值：" + reason);
            }
            if (error) *error = reason;
            return false;
        }
    }

    // 2) 当前玩家车辆的发动机对象：以定位器为准（换车/换发动机/读档必须能被发现），
    //    只有定位失败时才退回已保存并仍通过身份校验的对象。
    uint64_t target = 0;
    std::string resolveError;
    bool resolved = false;
    if (resolver_) resolved = resolver_->resolve(&target, &resolveError);
    if (!resolved) {
        if (activeObject_) {
            const auto it = objects_.find(activeObject_);
            if (it != objects_.end() && identityMatches(it->second)) target = activeObject_;
        }
        if (!target) {
            if (error) {
                *error = resolveError.empty() ? std::string("未绑定发动机定位器") : resolveError;
            }
            return false;
        }
    }

    // 活动对象为 0 的首次失败也会留下恢复任务；换对象不能绕开它。
    for (const auto& [address, state] : objects_) {
        if (!state.recoveryPending) continue;
        std::string detail;
        if (restoreObjectLocked(address, &detail) != RestoreOutcome::Ok) {
            if (error) *error = "部分事务恢复未确认，暂停写入：" + detail;
            return false;
        }
    }

    // 3) 换发动机对象（换车 / 换发动机 / 读档）：先尽力写回旧对象
    if (activeObject_ != 0 && target != activeObject_) {
        std::string detail;
        const RestoreOutcome outcome = restoreObjectLocked(activeObject_, &detail);
        if (outcome == RestoreOutcome::Failed) {
            if (error) *error = "切换发动机对象前无法写回旧对象原值：" + detail;
            return false;
        }
        if (outcome == RestoreOutcome::NotOurs) {
            objects_.erase(activeObject_);
            invalidatedRecord_ = true;  // 对象已明确失效：记录被丢弃
            bumpRevisionLocked();  // 待恢复记录集合变化
        }
        activeObject_ = 0;
    }

    // 4) 建立或复用基准
    EngineFieldValues current;
    if (!readEngineValues(*memory_, target, &current)) {
        if (error) *error = "读取发动机数据失败（对象可能已失效）";
        return false;
    }
    // 未确认的部分事务必须先安全恢复；不能把混合残留当作新的原厂基准。
    for (const RemoteCredential& credential : remoteCreds_) {
        if (credential.recoveryPending && !tunerSameTriple(current, credential.written) &&
            !tunerSameTriple(current, credential.base)) {
            if (error) *error = "上次部分事务尚未核对，当前内容不符；保留凭据并暂停写入";
            return false;
        }
    }
    ObjectState& state = baselineFor(target, current);
    if (state.recoveryPending) {
        std::string detail;
        if (restoreObjectLocked(target, &detail) != RestoreOutcome::Ok ||
            !readEngineValues(*memory_, target, &current)) {
            if (error) *error = "部分事务恢复未确认，暂停写入：" + detail;
            return false;
        }
    }

    // 5) 计算目标值并做安全范围检查
    EngineFieldValues values;
    if (!computeValuesLocked(state, scale, raiseLimit, &values, error)) return false;

    // 6) 写入 + 回读验证（任一环节失败都会回滚）
    if (!writeValuesLocked(target, state, values, error)) return false;

    state.written = values;
    state.hasWritten = true;
    state.recoveryPending = false;
    state.writtenScale = scale;
    state.writtenRaiseLimit = raiseLimit;
    if (activeObject_ != target) bumpRevisionLocked();  // 活动对象变化（含首次启用）
    activeObject_ = target;
    if (error) error->clear();
    return true;
}

EngineTuner::RestoreOutcome EngineTuner::restoreObjectLocked(uint64_t address,
                                                             std::string* detail) {
    auto it = objects_.find(address);
    if (it == objects_.end()) {
        if (detail) *detail = "没有该对象的原值记录";
        return RestoreOutcome::NotOurs;
    }
    ObjectState& state = it->second;
    if (!state.hasWritten) {
        if (detail) detail->clear();
        return RestoreOutcome::Ok;
    }
    if (!memory_ || !memory_->usable()) {
        if (detail) *detail = "游戏进程已不可用";
        return RestoreOutcome::Failed;
    }
    // 写入前重新验证对象身份：地址可能已被释放或被别的对象复用。
    EngineFieldValues current;
    if (!readEngineValues(*memory_, address, &current)) {
        if (detail) *detail = "发动机对象已失效（无法读取）";
        return state.recoveryPending ? RestoreOutcome::Failed : RestoreOutcome::NotOurs;
    }
    if (tunerSameTriple(current, state.base)) {
        state.hasWritten = false;
        state.recoveryPending = false;
        state.written = state.base;
        bumpRevisionLocked();  // 恢复成功：待恢复记录状态变化
        if (detail) detail->clear();
        return RestoreOutcome::Ok;
    }
    if (!tunerSameTriple(current, state.written)) {
        if (detail) *detail = "对象数值与本程序记录不符（地址可能已被复用）";
        invalidatedRecord_ = true;  // 对象已明确失效：记录不再适用（界面如实提示）
        return state.recoveryPending ? RestoreOutcome::Failed : RestoreOutcome::NotOurs;
    }
    std::string writeError;
    if (!writeValuesLocked(address, state, state.base, &writeError)) {
        if (detail) *detail = writeError;
        return RestoreOutcome::Failed;  // 记录原样保留（不删除恢复记录）
    }
    state.written = state.base;
    state.hasWritten = false;
    state.recoveryPending = false;
    bumpRevisionLocked();  // 恢复成功：待恢复记录状态变化
    if (detail) detail->clear();
    return RestoreOutcome::Ok;
}

bool EngineTuner::restoreActiveLocked(bool strict, std::string* error) {
    if (!activeObject_) {
        if (error) error->clear();
        return true;
    }
    const uint64_t object = activeObject_;
    std::string detail;
    const RestoreOutcome outcome = restoreObjectLocked(object, &detail);
    activeObject_ = 0;
    if (outcome == RestoreOutcome::Ok) {
        if (error) error->clear();
        return true;
    }
    if (outcome == RestoreOutcome::NotOurs) {
        objects_.erase(object);
        invalidatedRecord_ = true;  // 对象已明确失效：记录被丢弃（不谎称已恢复）
        bumpRevisionLocked();  // 待恢复记录集合变化
        if (error) *error = "未能写回原值：" + detail;
        return !strict;
    }
    if (error) *error = detail;
    return false;
}

bool EngineTuner::computeValuesLocked(const ObjectState& state, float scale, bool raiseLimit,
                                      EngineFieldValues* out, std::string* error) const {
    EngineFieldValues values = state.base;
    values.torque = state.base.torque * scale;
    if (state.useCopyA) values.copyA = state.base.copyA * scale;
    if (state.useCopyB) values.copyB = state.base.copyB * scale;
    if (raiseLimit) {
        values.rpmLimit = std::floor(state.base.rpmLimit * 1.10f + 0.5f);
        values.rpmLimitNeutral = std::floor(state.base.rpmLimitNeutral * 1.10f + 0.5f);
    } else {
        values.rpmLimit = state.base.rpmLimit;
        values.rpmLimitNeutral = state.base.rpmLimitNeutral;
    }

    if (!std::isfinite(values.torque) || values.torque < kMinWriteTorque ||
        values.torque > kMaxWriteTorque) {
        if (error) {
            *error = fmt("目标扭矩 %.1f N·m 超出安全范围（%.0f–%.0f），已拒绝写入",
                         (double)values.torque, (double)kMinWriteTorque,
                         (double)kMaxWriteTorque);
        }
        return false;
    }
    if (!std::isfinite(values.rpmLimit) || values.rpmLimit < kMinWriteRpm ||
        values.rpmLimit > (selectedGame() == GameId::Ats ? 10000.0f : kMaxWriteRpm) || values.rpmLimit <= state.base.rpmIdle) {
        if (error) *error = fmt("目标转速上限 %.1f 不合理，已拒绝写入", (double)values.rpmLimit);
        return false;
    }
    if (!std::isfinite(values.rpmLimitNeutral) || values.rpmLimitNeutral < kMinWriteRpm ||
        values.rpmLimitNeutral > (selectedGame() == GameId::Ats ? 10000.0f : kMaxWriteRpm)) {
        if (error) {
            *error = fmt("目标空挡转速上限 %.1f 不合理，已拒绝写入",
                         (double)values.rpmLimitNeutral);
        }
        return false;
    }
    if (state.useCopyA && (!std::isfinite(values.copyA) || values.copyA < kMinWriteTorque ||
                           values.copyA > kMaxWriteTorque)) {
        if (error) *error = "目标扭矩副本 A 超出安全范围，已拒绝写入";
        return false;
    }
    if (state.useCopyB && (!std::isfinite(values.copyB) || values.copyB < kMinWriteTorque ||
                           values.copyB > kMaxWriteTorque)) {
        if (error) *error = "目标扭矩副本 B 超出安全范围，已拒绝写入";
        return false;
    }
    if (out) *out = values;
    return true;
}

bool EngineTuner::writeValuesLocked(uint64_t address, ObjectState& state,
                                    const EngineFieldValues& values, std::string* error) {
    struct Field {
        uint64_t address;
        float    value;     // 目标值
        float    original;  // 回滚值
        float*   acknowledged = nullptr; // 本字段最近一次成功发出写入的值。
    };
    std::vector<Field> fields;
    fields.push_back({address + engine_runtime::kTorque, values.torque, state.base.torque});
    fields.push_back(
        {address + engine_runtime::kRpmLimit, values.rpmLimit, state.base.rpmLimit});
    fields.push_back({address + engine_runtime::kRpmLimitNeutral, values.rpmLimitNeutral,
                      state.base.rpmLimitNeutral});
    if (state.useCopyA) {
        fields.push_back({address + engine_runtime::kTorqueCurveTail, values.copyA, state.base.copyA});
    }
    if (state.useCopyB) {
        fields.push_back(
            {address + engine_runtime::kTorqueCurveTail + 4, values.copyB, state.base.copyB});
    }

    EngineFieldValues before;
    if (!readEngineValues(*memory_, address, &before)) {
        if (error) *error = "事务开始前读取发动机字段失败，未写入";
        return false;
    }
    EngineFieldValues expected = before;
    for (Field& field : fields) {
        const uint64_t offset = field.address - address;
        if (offset == engine_runtime::kTorque) field.acknowledged = &expected.torque;
        else if (offset == engine_runtime::kRpmLimit) field.acknowledged = &expected.rpmLimit;
        else if (offset == engine_runtime::kRpmLimitNeutral) field.acknowledged = &expected.rpmLimitNeutral;
        else if (offset == engine_runtime::kTorqueCurveTail) field.acknowledged = &expected.copyA;
        else field.acknowledged = &expected.copyB;
        field.original = *field.acknowledged; // 精确回滚到本轮之前，而非猜测全部已经原厂。
    }

    for (const Field& field : fields) {
        if (!std::isfinite(field.value)) {
            if (error) *error = "写入值不是有限数，已拒绝写入";
            return false;
        }
    }

    auto rollback = [&](size_t count, std::string* detail) {
        int failures = 0;
        for (size_t i = 0; i < count && i < fields.size(); ++i) {
            if (!tunerWriteFloat(*memory_, fields[i].address, fields[i].original)) ++failures;
            else *fields[i].acknowledged = fields[i].original;
        }
        std::string text;
        for (size_t i = 0; i < count && i < fields.size(); ++i) {
            float actual = 0.0f;
            if (!tunerReadFloat(*memory_, fields[i].address, &actual) ||
                !tunerClose(actual, fields[i].original, 0.5f)) {
                ++failures;
            }
        }
        if (failures > 0) {
            text = fmt("，回滚未完全成功（%d 项）", failures);
            // 不把未知读回值当作我们的值。只持久化逐字段成功 API 写入形成的图像。
            state.written = expected;
            state.hasWritten = true;
            state.recoveryPending = true;
            state.writtenScale = scale_;
            state.writtenRaiseLimit = raiseLimit_;
            bumpRevisionLocked();
        }
        if (detail) *detail = text;
    };

    for (size_t i = 0; i < fields.size(); ++i) {
        if (!tunerWriteFloat(*memory_, fields[i].address, fields[i].value)) {
            std::string rollbackNote;
            rollback(i + 1, &rollbackNote); // API 失败也可能留下部分字节；该字段同样核对回滚。
            if (error) {
                *error = fmt("写入字段 +0x%llX 失败%s",
                             (unsigned long long)(fields[i].address - address),
                             rollbackNote.c_str());
            }
            return false;
        }
        *fields[i].acknowledged = fields[i].value;
    }

    for (const Field& field : fields) {
        float actual = 0.0f;
        if (!tunerReadFloat(*memory_, field.address, &actual) ||
            !tunerClose(actual, field.value, 0.5f)) {
            std::string rollbackNote;
            rollback(fields.size(), &rollbackNote);
            if (error) {
                *error = fmt("回读校验失败：字段 +0x%llX 期望 %.3f 实际 %.3f%s",
                             (unsigned long long)(field.address - address), (double)field.value,
                             (double)actual, rollbackNote.c_str());
            }
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

EngineTuner::ObjectState& EngineTuner::baselineFor(uint64_t address,
                                                   const EngineFieldValues& current) {
    auto it = objects_.find(address);
    if (it == objects_.end()) {
        ObjectState state;
        state.address = address;
        state.base = current;
        // 跨进程重启的叠加保护：如果当前值正好是上次运行我们写入的值，
        // 就把当时记录的原厂基准当作基准，绝不把放大值当原厂值。
        // 逐条比对尚未采纳的导入记录：只采纳一次，采纳后立即移除并同步旧字段。
        for (size_t index = 0; index < remoteCreds_.size(); ++index) {
            const bool matchesBase = remoteCreds_[index].recoveryPending &&
                                     tunerSameTriple(current, remoteCreds_[index].base);
            if (!matchesBase && !tunerSameTriple(current, remoteCreds_[index].written)) continue;
            state.base = remoteCreds_[index].base;
            if (remoteCreds_[index].recoveryPending && !matchesBase) {
                state.written = remoteCreds_[index].written;
                state.hasWritten = true;
                state.recoveryPending = true;
                state.writtenScale = remoteCreds_[index].scale;
                state.writtenRaiseLimit = remoteCreds_[index].raiseLimit;
            }
            // v1 持久化格式不含这两个只读身份字段，不能用默认 0 取代实时值。
            state.base.rpmIdle = current.rpmIdle;
            state.base.resistance = current.resistance;
            remoteCreds_.erase(remoteCreds_.begin() + (std::ptrdiff_t)index);
            syncRemotePrimaryLocked();
            bumpRevisionLocked();  // 导入记录被采纳：恢复凭据状态变化
            break;
        }
        state.useCopyA = tunerSameMagnitude(state.base.copyA, state.base.torque);
        state.useCopyB = tunerSameMagnitude(state.base.copyB, state.base.torque);
        invalidatedRecord_ = false;  // 已建立新的有效记录
        bumpRevisionLocked();  // 新对象保存了原厂基准
        return objects_.emplace(address, state).first->second;
    }
    ObjectState& state = it->second;
    if (state.recoveryPending) return state;
    if (state.hasWritten) {
        if (tunerSameTriple(current, state.written)) return state;  // 仍是我们写入的值
        if (tunerSameTriple(current, state.base)) {                 // 游戏已把它改回原厂
            state.hasWritten = false;
            state.written = state.base;
            bumpRevisionLocked();  // 恢复状态变化（游戏侧已回原厂）
            return state;
        }
        state.base = current;  // 其它数值 → 视为新的原厂值
        state.hasWritten = false;
        bumpRevisionLocked();  // 基准值更新
    } else if (!tunerSameTriple(current, state.base)) {
        state.base = current;  // 同一地址被新的发动机对象复用
        bumpRevisionLocked();  // 基准值更新
    }
    state.written = state.base;
    state.useCopyA = tunerSameMagnitude(state.base.copyA, state.base.torque);
    state.useCopyB = tunerSameMagnitude(state.base.copyB, state.base.torque);
    return state;
}

bool EngineTuner::identityMatches(const ObjectState& state) const {
    if (!memory_ || !memory_->usable() || state.address == 0) return false;
    EngineFieldValues current;
    if (!readEngineValues(*memory_, state.address, &current)) return false;
    const bool looksOurs = tunerSameTriple(current, state.written) ||
                           tunerSameTriple(current, state.base);
    if (!looksOurs) return false;
    // rpmIdle 与阻力扭矩不随本程序的写入变化，用来确认还是同一个对象。
    if (!tunerClose(current.rpmIdle, state.base.rpmIdle, 1.0f)) return false;
    if (!tunerClose(current.resistance, state.base.resistance, 1.0f)) return false;
    return true;
}

std::string EngineTuner::summaryLocked() const {
    std::string text = fmt("动力 %.2f× 保持中", (double)scale_);
    if (activeObject_) {
        const auto it = objects_.find(activeObject_);
        if (it != objects_.end()) {
            text += fmt("（原厂 %.0f N·m → 当前 %.0f N·m）", (double)it->second.base.torque,
                        (double)it->second.base.torque * (double)scale_);
        }
    }
    if (raiseLimit_) text += "，转速上限 +10%";
    return text;
}

float EngineTuner::scale() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scale_;
}

bool EngineTuner::raiseLimit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raiseLimit_;
}

bool EngineTuner::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool EngineTuner::bound() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return memory_ != nullptr;
}

uint64_t EngineTuner::activeObject() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeObject_;
}

uint64_t EngineTuner::stateRevision() const {
    // 无锁读取（原子）：界面线程每帧轮询。修订号在 mutex_ 内、状态变更落定前自增，
    // 因此读到新修订号后再加锁调用 exportState() 必然拿到新状态。
    return stateRevision_.load(std::memory_order_acquire);
}

std::string EngineTuner::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool EngineTuner::baselineOf(uint64_t object, EngineFieldValues* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = objects_.find(object);
    if (it == objects_.end()) return false;
    if (out) *out = it->second.base;
    return true;
}

std::string EngineTuner::exportState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // 状态机（导出优先序）：
    //   1) 已修改且尚未确认恢复的对象记录（objects_ 中 hasWritten）→ 主记录
    //   2) 尚未核对的导入记录（remoteCreds_）→ 同样的主记录格式（绝不导出为空，
    //      否则自动保存会把上次运行的恢复凭据覆盖掉）
    //   3) 两者都有 → 主记录 = 对象记录，其余未采纳的导入记录以 ";P;..." 追加
    //      （旧版本读到时只会取前 11 个数字，天然向后兼容）
    //   4) 已确认恢复（无对象记录、无导入记录）→ 空串，避免留下过期凭据
    std::string primary;
    const auto it = activeObject_ ? objects_.find(activeObject_) : objects_.end();
    const ObjectState* chosen = nullptr;
    if (it != objects_.end() && it->second.hasWritten) {
        chosen = &it->second;
    } else {
        for (const auto& [address, state] : objects_) {
            if (state.hasWritten) {
                chosen = &state;
                break;
            }
        }
    }
    auto formatRecord = [](const EngineFieldValues& base, const EngineFieldValues& written,
                           float recordScale, bool recordRaiseLimit) {
        return fmt("1;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.3f;%d",
                   base.torque, base.copyA, base.copyB, base.rpmLimit, base.rpmLimitNeutral,
                   written.torque, written.copyA, written.copyB, written.rpmLimit,
                   written.rpmLimitNeutral, (double)recordScale, recordRaiseLimit ? 1 : 0);
    };
    size_t credentialStart = 0;
    if (chosen) {
        primary = formatRecord(chosen->base, chosen->written, chosen->writtenScale,
                               chosen->writtenRaiseLimit);
        if (chosen->recoveryPending) primary += ";U";
    } else if (!remoteCreds_.empty()) {
        const RemoteCredential& credential = remoteCreds_[0];
        primary = formatRecord(credential.base, credential.written, credential.scale,
                               credential.raiseLimit);
        if (credential.recoveryPending) primary += ";U";
        credentialStart = 1;
    }
    for (size_t i = credentialStart; i < remoteCreds_.size(); ++i) {
        const RemoteCredential& credential = remoteCreds_[i];
        primary += fmt(";P;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.3f;%d",
                       credential.base.torque, credential.base.copyA, credential.base.copyB,
                       credential.base.rpmLimit, credential.base.rpmLimitNeutral,
                       credential.written.torque, credential.written.copyA,
                       credential.written.copyB, credential.written.rpmLimit,
                       credential.written.rpmLimitNeutral, (double)credential.scale,
                       credential.raiseLimit ? 1 : 0);
        if (credential.recoveryPending) primary += ";U";
    }
    return primary;
}

void EngineTuner::importState(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteCreds_.clear();
    remoteRecordValid_ = false;
    remoteRecordActive_ = false;
    // 格式（向后兼容）：
    //   主记录   "1;<base5>;<written5>;<scale>;<raise>"
    //   追加记录 ";P;<base5>;<written5>;<scale>;<raise>"（零到多条，尚未核对的导入）
    // 旧版本读到追加段只会取前若干个数字，不会误解；新版本读旧格式也完全一致。
    // 分段规则：
    //   * "P" 是**任何解析状态下都识别**的分段标记（旧实现只在首个 token 前识别，
    //     导致第二条及之后的凭据被并入第一条并丢失）；
    //   * "1" 只在一段的起始位置充当标志位，避免把数据里的 1（例如 raise=1）当标志。
    std::vector<RemoteCredential> parsed;
    std::vector<float> numbers;
    bool hasFlag = false;
    bool atSegmentStart = true;
    bool recoveryPending = false;
    auto flush = [&]() {
        if (numbers.size() >= 11) {
            RemoteCredential credential;
            credential.base.torque = numbers[0];
            credential.base.copyA = numbers[1];
            credential.base.copyB = numbers[2];
            credential.base.rpmLimit = numbers[3];
            credential.base.rpmLimitNeutral = numbers[4];
            credential.written.torque = numbers[5];
            credential.written.copyA = numbers[6];
            credential.written.copyB = numbers[7];
            credential.written.rpmLimit = numbers[8];
            credential.written.rpmLimitNeutral = numbers[9];
            credential.scale = numbers[10];
            credential.raiseLimit = numbers.size() > 11 && numbers[11] != 0.0f;
            credential.recoveryPending = recoveryPending;
            parsed.push_back(credential);
        }
        numbers.clear();
        atSegmentStart = true;
        recoveryPending = false;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t sep = text.find(';', pos);
        const std::string token =
            text.substr(pos, (sep == std::string::npos) ? std::string::npos : sep - pos);
        if (!token.empty()) {
            if (token == "P") {
                // 分段标记：无论当前在什么位置都要结束上一段并开启新段
                flush();
            } else if (token == "U") {
                recoveryPending = true; // 可选尾标记：部分事务须先恢复，不能作为新基准。
            } else if (atSegmentStart && token == "1" && numbers.empty()) {
                hasFlag = true;
                atSegmentStart = false;
            } else if (atSegmentStart) {
                // 段首直接是数字（无标志位）：按数字继续解析该段
                atSegmentStart = false;
                numbers.push_back((float)::atof(token.c_str()));
            } else {
                numbers.push_back((float)::atof(token.c_str()));
            }
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    flush();
    if (!hasFlag || parsed.empty()) {
        return;  // 没有可用的恢复记录：保持空（不虚报、不留下过期凭据）
    }
    remoteCreds_ = std::move(parsed);
    // 第一条作为主凭据（与旧版语义一致：用来抵消上一次运行的放大值）
    remoteBase_ = remoteCreds_[0].base;
    remoteWritten_ = remoteCreds_[0].written;
    remoteRecordValid_ = true;
    remoteRecordActive_ = true;
    bumpRevisionLocked();  // 导入了待核对的上次运行记录（UI 会立即持久化，不再丢失）
}

void EngineTuner::setBackgroundThreadEnabled(bool enabled) {
    backgroundEnabled_.store(enabled);
}

void EngineTuner::setGuardIntervalMs(uint64_t milliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    guardIntervalMs_ = milliseconds;
}

void EngineTuner::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(waitMutex_);
            waitCv_.wait_for(lock, std::chrono::milliseconds(kMaintenanceIntervalMs),
                             [this]() { return threadStop_.load(); });
            if (threadStop_.load()) break;
        }
        if (!backgroundEnabled_.load()) continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_ || !memory_) continue;
        }
        std::string error;
        try {
            // 异常隔离：任何异常都不允许逃出线程函数（否则 std::terminate 会带走
            // 整个修改器，且此时游戏里可能仍留着放大值）。停用并保留恢复记录，
            // 由退出路径的 restoreAndDetach / 下次启动的跨会话记录继续兜底。
            maintenance(&error);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = false;
            setStatusLocked(fmt("动力调节后台线程异常，已停止（恢复记录已保留）：%s", e.what()));
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = false;
            setStatusLocked("动力调节后台线程发生未知异常，已停止（恢复记录已保留）");
        }
    }
}

void EngineTuner::setStatusLocked(const std::string& text) { status_ = text; }

void EngineTuner::syncRemotePrimaryLocked() {
    // 维护「主凭据」视图（remoteBase_/remoteWritten_/remoteRecordValid_），
    // 供既有的 status/日志逻辑使用；真正的持久化以 remoteCreds_ 全表为准。
    remoteRecordValid_ = !remoteCreds_.empty();
    if (remoteCreds_.empty()) {
        remoteBase_ = EngineFieldValues{};
        remoteWritten_ = EngineFieldValues{};
        remoteRecordActive_ = false;
        return;
    }
    remoteBase_ = remoteCreds_[0].base;
    remoteWritten_ = remoteCreds_[0].written;
    remoteRecordActive_ = true;
}

bool EngineTuner::hasUnverifiedImportedRecord() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !remoteCreds_.empty();
}

bool EngineTuner::hasInvalidatedRecord() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return invalidatedRecord_;
}

void EngineTuner::bumpRevisionLocked() {
    stateRevision_.store(stateRevision_.load(std::memory_order_relaxed) + 1,
                         std::memory_order_release);
}

bool probeAccessories(const ProcessMemory& memory,
                      const std::vector<std::pair<std::string, uint64_t>>& roots,
                      AccessoryProbeResult* result, std::string* error) {
    if (!memory.isOpen()) {
        if (error) *error = "游戏进程尚未附加";
        return false;
    }
    if (!result) {
        if (error) *error = "结果指针为空";
        return false;
    }
    *result = AccessoryProbeResult();

    std::vector<Node> nodes;
    std::unordered_set<uint64_t> seen;
    std::unordered_set<uint64_t> engineHits;
    std::unordered_set<uint64_t> transmissionHits;
    std::unordered_set<uint64_t> stringProbed;

    for (const auto& root : roots) {
        if (!isCanonical(root.second) || seen.count(root.second)) continue;
        Node node;
        node.address = root.second;
        node.depth = 0;
        node.parent = -1;
        node.offset = 0;
        node.label = root.first;
        seen.insert(node.address);
        nodes.push_back(node);
    }
    if (nodes.empty()) {
        if (error) *error = "没有可用的探针根节点";
        return false;
    }

    std::vector<uint8_t> buffer((size_t)kRootWindow, 0);
    for (size_t index = 0; index < nodes.size() && index < kMaxNodes; ++index) {
        const Node node = nodes[index];
        const uint64_t windowSize = node.depth == 0 ? kRootWindow : kChildWindow;
        if (result->bytesRead + windowSize > kMaxBytes) {
            result->note = "已达到读取上限，提前结束遍历；";
            break;
        }
        size_t got = 0;
        if (!memory.read(node.address, buffer.data(), (size_t)windowSize, &got) || got < 16) {
            continue;
        }
        result->bytesRead += got;
        ++result->nodesVisited;

        // 1) 名称字符串识别：带 @@...@@ 或 /def/... 的节点基本就是附属总成数据对象。
        std::vector<std::pair<uint64_t, std::string>> names;
        scanWindowStrings(memory, buffer, got, kStringWindow, &stringProbed, &names);
        if (!names.empty()) {
            std::string joined;
            for (const auto& item : names) joined += lowerCase(item.second) + " ";
            const bool interesting = joined.find("engine") != std::string::npos ||
                                     joined.find("transmission") != std::string::npos ||
                                     joined.find("@@") != std::string::npos;
            if (interesting && result->named.size() < kMaxNamed) {
                NamedNode named;
                named.object = node.address;
                named.depth = node.depth;
                named.path = buildPath(nodes, index);
                named.strings = names;
                dumpFloatsAndPointers(memory, node.address, kDumpBegin, kDumpEnd, &named.floats,
                                      &named.pointers);
                result->named.push_back(std::move(named));
            }
        }

        // 2) 字段签名识别。
        AccessoryHit hit;
        hit.object = node.address;
        hit.depth = node.depth;
        if (!engineHits.count(node.address) && looksLikeEngineData(memory, node.address, &hit)) {
            hit.path = buildPath(nodes, index);
            if (node.parent >= 0) {
                collectFamily(memory, nodes[(size_t)node.parent].address, &hit.family);
            }
            dumpFloatsAndPointers(memory, node.address, kDumpBegin, kDumpEnd, &hit.floats,
                                  &hit.pointers);
            engineHits.insert(node.address);
            result->engines.push_back(std::move(hit));
        } else if (!transmissionHits.count(node.address) &&
                   looksLikeTransmissionData(memory, node.address, &hit)) {
            hit.path = buildPath(nodes, index);
            if (node.parent >= 0) {
                collectFamily(memory, nodes[(size_t)node.parent].address, &hit.family);
            }
            dumpFloatsAndPointers(memory, node.address, kDumpBegin, kDumpEnd, &hit.floats,
                                  &hit.pointers);
            transmissionHits.insert(node.address);
            result->transmissions.push_back(std::move(hit));
        }

        if (node.depth >= kMaxDepth) continue;
        for (size_t offset = 0; offset + 8 <= got; offset += 8) {
            uint64_t pointer = 0;
            ::memcpy(&pointer, buffer.data() + offset, sizeof(pointer));
            if (!isCanonical(pointer) || seen.count(pointer)) continue;
            if (nodes.size() >= kMaxNodes) break;
            Node child;
            child.address = pointer;
            child.depth = node.depth + 1;
            child.parent = (int)index;
            child.offset = offset;
            seen.insert(pointer);
            nodes.push_back(child);
        }
    }

    result->note += fmt("遍历节点 %llu 个、读取 %s；发动机候选 %d、变速箱候选 %d、具名对象 %d",
                        (unsigned long long)result->nodesVisited,
                        formatSize(result->bytesRead).c_str(), (int)result->engines.size(),
                        (int)result->transmissions.size(), (int)result->named.size());
    if (error) error->clear();
    return true;
}

}  // namespace ets2
