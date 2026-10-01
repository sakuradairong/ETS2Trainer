// vehicle.cpp —— ETS2 1.61.1.1 固定入口；运行时只沿 4 级指针链读取几十字节
#include "vehicle.h"

#include "gameio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace ets2 {

namespace {

// eurotrucks2.exe 1.61.1.1 Steam x64。
// 该指令是 `mov rbx,[game_root]`，其 RIP 相对目标为当前游戏根全局。
constexpr uint64_t kRootInstructionRva = 0x5F8B6A;
const uint8_t kRootInstruction[] = {
    0x48, 0x8B, 0x1D, 0x67, 0x5B, 0x0B, 0x03, 0x48, 0x8B,
    0xF9, 0x48, 0x8B, 0x9B, 0xB0, 0x31, 0x00, 0x00,
};

constexpr uint64_t kRootToDriver = 0x31B0;
constexpr uint64_t kDriverToContext = 0x18;
constexpr uint64_t kContextToTruck = 0x1F8;
constexpr uint64_t kContextFuelCorrection = 0x1158;

constexpr uint64_t kTruckCenterOfMassY = 0x468;
constexpr uint64_t kTruckChassisDescriptor = 0x200;
constexpr uint64_t kChassisDefaultCoMHeight = 0x62C;
constexpr uint64_t kCvarTruckStabilityFloat = 0x02D470A0;
constexpr uint64_t kCvarTrailerStabilityFloat = 0x02D47320;
constexpr uint64_t kTruckFuelCapacity = 0x190;
constexpr uint64_t kTruckFuelRatio = 0x1B8;
constexpr uint64_t kTruckChassisPermanent = 0x98;
constexpr uint64_t kTruckChassisWear = 0x9C;
constexpr uint64_t kTruckEnginePermanent = 0x1A0;
constexpr uint64_t kTruckTransmissionPermanent = 0x1A4;
constexpr uint64_t kTruckCabinPermanent = 0x1A8;
constexpr uint64_t kTruckEngineWear = 0x1AC;
constexpr uint64_t kTruckTransmissionWear = 0x1B0;
constexpr uint64_t kTruckCabinWear = 0x1B4;
constexpr uint64_t kTruckWheelWearArray = 0xA8;
constexpr uint64_t kTruckWheelWearCount = 0xB0;
constexpr uint64_t kTruckWheelPermanentArray = 0xD0;
constexpr uint64_t kTruckWheelPermanentCount = 0xD8;
constexpr uint64_t kTruckWheelWearCache = 0x168;
constexpr uint64_t kTruckWheelPermanentCache = 0x16C;
constexpr uint64_t kMaxWheelFields = 64;

bool isCanonicalPointer(uint64_t value) {
    return value >= 0x10000ull && value <= 0x00007FFFFFFFFFFFull;
}

bool readPointer(const ProcessMemory& memory, uint64_t address, uint64_t* value) {
    uint64_t local = 0;
    if (!memory.read(address, &local, sizeof(local)) || !isCanonicalPointer(local)) return false;
    *value = local;
    return true;
}

bool readFloat(const ProcessMemory& memory, uint64_t address, float* value) {
    float local = 0.0f;
    if (!memory.read(address, &local, sizeof(local)) || !std::isfinite(local)) return false;
    *value = local;
    return true;
}

// 保守范围：包含本程序的目标 [-6,-0.2] 与常规车辆米级重心偏移。
// 布局/对象不匹配时，即使是有限 float 也不能当作可修改的物理字段。
bool plausibleCenterOfMass(float value) {
    return std::isfinite(value) && value >= -6.0f && value <= 5.0f;
}

bool closeFloat(float a, float b, float tolerance) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tolerance;
}

float clampWear(float value) {
    return (std::max)(0.0f, (std::min)(1.0f, value));
}

bool readWheelGroup(const ProcessMemory& memory, uint64_t truck, uint64_t pointerOffset,
                    uint64_t countOffset, uint64_t cacheOffset, float* value,
                    std::vector<uint64_t>* fields) {
    uint64_t count = 0;
    if (!memory.read(truck + countOffset, &count, sizeof(count)) || count > kMaxWheelFields) {
        return false;
    }
    uint64_t array = 0;
    if (count > 0 && !readPointer(memory, truck + pointerOffset, &array)) return false;

    float sum = 0.0f;
    for (uint64_t index = 0; index < count; ++index) {
        float item = 0.0f;
        const uint64_t address = array + index * sizeof(float);
        if (!readFloat(memory, address, &item) || item < -0.001f || item > 1.001f) return false;
        sum += item;
        if (fields) fields->push_back(address);
    }
    float cached = -1.0f;
    if (!readFloat(memory, truck + cacheOffset, &cached)) return false;
    *value = cached >= 0.0f ? cached : (count ? sum / static_cast<float>(count) : 0.0f);
    if (fields) fields->push_back(truck + cacheOffset);
    return true;
}

}  // namespace

// ===========================================================================
// AntiRollBackupManager：防侧翻原始值备份（每辆车一条记录，逐项验证清除）
// ===========================================================================
bool AntiRollBackupManager::writeFloatVerified(ProcessMemory& memory, uint64_t address,
                                               float value) {
    if (!memory.isOpen()) return false;
    if (!memory.write(address, &value, sizeof(value))) return false;
    float readBack = 0.0f;
    if (!memory.read(address, &readBack, sizeof(readBack))) return false;
    if (!std::isfinite(readBack) || std::fabs(readBack - value) > 1e-6f) return false;
    return true;
}

void AntiRollBackupManager::quarantineLocked(const AntiRollRecord& record,
                                             const AntiRollItem& item,
                                             const std::string& reason) {
    AntiRollQuarantine entry;
    entry.generation = record.generation;
    entry.truckObject = record.truckObject;
    entry.address = item.address;
    entry.original = item.original;
    entry.applied = item.applied;
    entry.reason = reason;
    quarantine_.push_back(std::move(entry));
    ++quarantineTotal_;  // 单调计数：不受诊断队列长度上限影响
    if (quarantine_.size() > kMaxQuarantine) quarantine_.erase(quarantine_.begin());
}

void AntiRollBackupManager::quarantineItemLocked(AntiRollRecord* record, AntiRollItem* item,
                                                 const std::string& reason) {
    quarantineLocked(*record, *item, reason);
    item->state = AntiRollItemState::Quarantined;
}

size_t AntiRollBackupManager::beginProcessGeneration(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t moved = 0;
    for (const AntiRollRecord& record : vehicles_) {
        for (const AntiRollItem* item : {&record.com, &record.truckCvar, &record.trailerCvar}) {
            if (item->active()) {
                quarantineLocked(record, *item,
                                 "进程代次已变化：旧进程的恢复地址不得在新进程重放");
                ++moved;
            }
        }
    }
    for (const AntiRollItem* item : {&cvar_.truckCvar, &cvar_.trailerCvar}) {
        if (item->active()) {
            quarantineLocked(cvar_, *item,
                             "进程代次已变化：旧进程的 cvar 恢复地址不得在新进程重放");
            ++moved;
        }
    }
    vehicles_.clear();
    cvar_ = AntiRollRecord{};
    generation_ = generation;
    return moved;
}

uint64_t AntiRollBackupManager::processGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool AntiRollBackupManager::captureCenterOfMass(ProcessMemory& memory, uint64_t truckObject,
                                                uint64_t comAddress,
                                                const VehicleIdentity& identity,
                                                std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& existing : vehicles_) {
        if (existing.truckObject == truckObject && existing.com.active()) {
            if (existing.identityKnown && existing.identity != identity) {
                // 地址相同但身份不符：说明这个地址现在是别的对象 —— 隔离旧记录，
                // 绝不用旧记录的原值去覆盖新对象。
                quarantineItemLocked(&existing, &existing.com,
                                     "车辆身份与记录不符（地址可能已被复用）");
                for (AntiRollItem* item : {&existing.truckCvar, &existing.trailerCvar}) {
                    if (item->active()) {
                        quarantineItemLocked(&existing, item,
                                             "车辆身份与记录不符（地址可能已被复用）");
                    }
                }
            } else {
                // 同一车辆已有备份：沿用原始值，绝不把当前（可能已被修改）的值存成原值
                if (error) error->clear();
                return true;
            }
        }
    }
    // 清掉已无有效项的记录
    for (size_t index = 0; index < vehicles_.size();) {
        const AntiRollRecord& record = vehicles_[index];
        if (!record.com.active() && !record.truckCvar.active() &&
            !record.trailerCvar.active()) {
            vehicles_.erase(vehicles_.begin() + (std::ptrdiff_t)index);
        } else {
            ++index;
        }
    }
    if (vehicles_.size() >= kMaxVehicleRecords) {
        // 超出上限：拒绝新建，但保留现有记录（不静默丢弃、不用新值覆盖）
        if (error) {
            *error = fmt("待恢复的车辆记录已达上限（%d 辆），拒绝新增备份",
                         (int)kMaxVehicleRecords);
        }
        return false;
    }
    float current = 0.0f;
    if (!memory.isOpen() ||
        !memory.read(comAddress, &current, sizeof(current)) || !plausibleCenterOfMass(current)) {
        if (error) *error = "车辆重心偏移不可读或超出保守范围 [-6,5]，拒绝备份与写入";
        return false;
    }
    AntiRollRecord record;
    record.generation = generation_;
    record.truckObject = truckObject;
    record.identity = identity;
    record.identityKnown = true;
    record.com.address = comAddress;
    record.com.original = current;
    record.com.applied = 0.0f;
    record.com.state = AntiRollItemState::Captured;
    // 继承进程级 cvar 备份状态（cvar 与具体车辆无关，但同属一个进程代次）
    record.truckCvar = cvar_.truckCvar;
    record.trailerCvar = cvar_.trailerCvar;
    for (AntiRollItem* item : {&record.truckCvar, &record.trailerCvar}) {
        if (item->state == AntiRollItemState::Restored) *item = AntiRollItem{};
    }
    vehicles_.push_back(record);
    if (error) error->clear();
    return true;
}

bool AntiRollBackupManager::captureCvars(ProcessMemory& memory, uint64_t truckCvar,
                                         uint64_t trailerCvar, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cvar_.truckCvar.state == AntiRollItemState::Quarantined ||
        cvar_.trailerCvar.state == AntiRollItemState::Quarantined) {
        if (error) *error = "稳定性 cvar 已隔离，当前进程代次不重新捕获或写入";
        return false;
    }
    if (cvar_.truckCvar.active() && cvar_.trailerCvar.active()) {
        if (cvar_.truckCvar.address != truckCvar || cvar_.trailerCvar.address != trailerCvar) {
            if (error) *error = "稳定性 cvar 地址变化，保留已有恢复凭据";
            return false;
        }
        if (error) error->clear();
        return true;
    }
    if (!truckCvar || !trailerCvar) {
        if (error) *error = "稳定性 cvar 地址不可用";
        return false;
    }
    float truck = 0.0f, trailer = 0.0f;
    if (!memory.isOpen() || !readFloat(memory, truckCvar, &truck) ||
        !readFloat(memory, trailerCvar, &trailer)) {
        if (error) *error = "读取稳定性 cvar 失败";
        return false;
    }
    cvar_.generation = generation_;
    cvar_.truckObject = 0;
    cvar_.identityKnown = false;
    if ((cvar_.truckCvar.active() && cvar_.truckCvar.address != truckCvar) ||
        (cvar_.trailerCvar.active() && cvar_.trailerCvar.address != trailerCvar)) {
        if (error) *error = "稳定性 cvar 地址变化，保留已有恢复凭据";
        return false;
    }
    auto capture = [](AntiRollItem& item, uint64_t address, float original) {
        if (item.active()) return; // 另一个字段恢复成功不允许覆盖仍待恢复的凭据。
        item = AntiRollItem{};
        item.address = address;
        item.original = original;
        item.state = AntiRollItemState::Captured;
    };
    capture(cvar_.truckCvar, truckCvar, truck);
    capture(cvar_.trailerCvar, trailerCvar, trailer);
    // 已存在的车辆记录同样获得 cvar 备份（还原时逐项处理）
    for (AntiRollRecord& record : vehicles_) {
        if (record.truckCvar.state == AntiRollItemState::None ||
            record.truckCvar.state == AntiRollItemState::Restored) record.truckCvar = cvar_.truckCvar;
        if (record.trailerCvar.state == AntiRollItemState::None ||
            record.trailerCvar.state == AntiRollItemState::Restored) record.trailerCvar = cvar_.trailerCvar;
    }
    if (error) error->clear();
    return true;
}

// 兼容重载：自行读取身份指纹；读不到身份时拒绝记录（宁可不备份，也不用未知身份写入）。
bool AntiRollBackupManager::captureCenterOfMass(ProcessMemory& memory, uint64_t truckObject,
                                                uint64_t comAddress, std::string* error) {
    VehicleIdentity identity;
    if (!readVehicleIdentity(memory, truckObject, &identity)) {
        if (error) *error = "无法读取车辆身份指纹（底盘描述符/油箱容量/磨损数组），拒绝建立备份";
        return false;
    }
    return captureCenterOfMass(memory, truckObject, comAddress, identity, error);
}

void AntiRollBackupManager::setVerifiedValue(AntiRollItem& item, float value) {
    if (!item.active() || !std::isfinite(value)) return;
    item.applied = value;
    item.hasApplied = !closeFloat(value, item.original, 1e-6f);
    item.hasAcknowledged = false;
    item.state = item.hasApplied ? AntiRollItemState::Applied : AntiRollItemState::Captured;
}

void AntiRollBackupManager::verifyRecordedValue(AntiRollItem& item, float value) {
    if (closeFloat(value, item.original, 1e-6f) ||
        (item.hasApplied && closeFloat(value, item.applied, 1e-4f)) ||
        (item.hasAcknowledged && closeFloat(value, item.acknowledged, 1e-4f))) {
        setVerifiedValue(item, value);
    }
}

void AntiRollBackupManager::requestRestore(AntiRollItem& item) {
    if (item.active() && (item.hasApplied || item.hasAcknowledged)) {
        item.state = AntiRollItemState::NeedsRestore;
    }
}

void AntiRollBackupManager::forItemsAtAddressLocked(
    uint64_t address, const std::function<void(AntiRollItem&)>& update) {
    for (AntiRollRecord& record : vehicles_) {
        for (AntiRollItem* item : {&record.com, &record.truckCvar, &record.trailerCvar}) {
            if (item->active() && item->address == address) update(*item);
        }
    }
    for (AntiRollItem* item : {&cvar_.truckCvar, &cvar_.trailerCvar}) {
        if (item->active() && item->address == address) update(*item);
    }
}

void AntiRollBackupManager::noteFieldWriteSucceeded(uint64_t address, float value) {
    if (!std::isfinite(value)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    forItemsAtAddressLocked(address, [value](AntiRollItem& item) {
        item.acknowledged = value;
        item.hasAcknowledged = true;
        requestRestore(item);
    });
}

void AntiRollBackupManager::noteFieldValueVerified(uint64_t address, float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    forItemsAtAddressLocked(address, [value](AntiRollItem& item) {
        verifyRecordedValue(item, value);
    });
}

void AntiRollBackupManager::markFieldPendingRestore(uint64_t address) {
    std::lock_guard<std::mutex> lock(mutex_);
    forItemsAtAddressLocked(address, [](AntiRollItem& item) { requestRestore(item); });
}

void AntiRollBackupManager::noteAppliedCenterOfMass(uint64_t truckObject, float comAppliedY) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& record : vehicles_) {
        if (record.truckObject != truckObject) continue;
        setVerifiedValue(record.com, comAppliedY);
    }
}

void AntiRollBackupManager::noteAppliedStability(float truckStabilityApplied,
                                                 float trailerStabilityApplied) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& record : vehicles_) {
        if (truckStabilityApplied != 0.0f) setVerifiedValue(record.truckCvar, truckStabilityApplied);
        if (trailerStabilityApplied != 0.0f) setVerifiedValue(record.trailerCvar, trailerStabilityApplied);
    }
    if (truckStabilityApplied != 0.0f) setVerifiedValue(cvar_.truckCvar, truckStabilityApplied);
    if (trailerStabilityApplied != 0.0f) setVerifiedValue(cvar_.trailerCvar, trailerStabilityApplied);
}

void AntiRollBackupManager::noteApplied(uint64_t truckObject, float comAppliedY,
                                        float truckStabilityApplied,
                                        float trailerStabilityApplied) {
    // 兼容重载：0.0f = 该字段本轮未写入（合法写入值永远不可能是 0，见头文件说明）
    if (comAppliedY != 0.0f) noteAppliedCenterOfMass(truckObject, comAppliedY);
    if (truckStabilityApplied != 0.0f || trailerStabilityApplied != 0.0f) {
        noteAppliedStability(truckStabilityApplied, trailerStabilityApplied);
    }
}

void AntiRollBackupManager::markCenterOfMassPendingRestore(uint64_t truckObject,
                                                            float appliedValue) {
    (void)appliedValue; // 恢复请求本身不产生成功写入的凭据。
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& record : vehicles_) {
        if (record.truckObject != truckObject) continue;
        requestRestore(record.com);
    }
}

void AntiRollBackupManager::markStabilityPendingRestore(float truckApplied, float trailerApplied) {
    (void)truckApplied;
    (void)trailerApplied;
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& record : vehicles_) {
        requestRestore(record.truckCvar);
        requestRestore(record.trailerCvar);
    }
    requestRestore(cvar_.truckCvar);
    requestRestore(cvar_.trailerCvar);
}

void AntiRollBackupManager::noteCenterOfMassValueInMemory(uint64_t truckObject,
                                                          float valueInMemory) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& record : vehicles_) {
        if (record.truckObject != truckObject) continue;
        verifyRecordedValue(record.com, valueInMemory);
    }
}

void AntiRollBackupManager::noteStabilityValuesInMemory(float truckValueInMemory,
                                                        float trailerValueInMemory) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& record : vehicles_) {
        verifyRecordedValue(record.truckCvar, truckValueInMemory);
        verifyRecordedValue(record.trailerCvar, trailerValueInMemory);
    }
    verifyRecordedValue(cvar_.truckCvar, truckValueInMemory);
    verifyRecordedValue(cvar_.trailerCvar, trailerValueInMemory);
}

void AntiRollBackupManager::markNeedsRestore(uint64_t truckObject) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (AntiRollRecord& record : vehicles_) {
        if (record.truckObject != truckObject) continue;
        for (AntiRollItem* item : {&record.com, &record.truckCvar, &record.trailerCvar}) {
            requestRestore(*item);
        }
    }
    for (AntiRollItem* item : {&cvar_.truckCvar, &cvar_.trailerCvar}) {
        requestRestore(*item);
    }
}

// 单项恢复。Restored=已还原 / Retry=暂时不可读需重试 / Quarantined=无法确认已隔离。
AntiRollBackupManager::ItemOutcome AntiRollBackupManager::restoreItemLocked(
    ProcessMemory& memory, AntiRollRecord& record, AntiRollItem* item, bool needIdentity,
    std::string* detail) {
    if (!item->active() || !item->address) return ItemOutcome::Restored;
    if (record.generation != generation_) {
        quarantineItemLocked(&record, item, "进程代次与当前不同：拒绝向新进程重放旧地址");
        if (detail) *detail += "进程代次不符（已隔离，未写入）；";
        return ItemOutcome::Quarantined;
    }
    if (needIdentity && record.identityKnown) {
        VehicleIdentity current;
        if (!readVehicleIdentity(memory, record.truckObject, &current)) {
            // 对象暂时读不到（读档中/换图瞬间）：绝不写入，保留记录稍后重试
            if (detail) *detail += "车辆对象暂时不可读（稍后重试，未写入）；";
            return ItemOutcome::Retry;
        }
        if (current != record.identity) {
            quarantineItemLocked(&record, item, "车辆身份与记录不符（地址可能已被复用）");
            if (detail) *detail += "车辆身份不符（已隔离，未写入）；";
            return ItemOutcome::Quarantined;
        }
    }
    float currentValue = 0.0f;
    if (!memory.isOpen() || !readFloat(memory, item->address, &currentValue)) {
        // 地址暂时不可读：不把「重新可读」当成旧对象重新出现，仍然要复核身份/内容
        if (detail) *detail += "目标字段暂时不可读（稍后重试，未写入）；";
        return ItemOutcome::Retry;
    }
    if (closeFloat(currentValue, item->original, 1e-6f)) {
        // 已经是原值：无需写入，直接视为已还原（不重复覆盖）
        item->state = AntiRollItemState::Restored;
        return ItemOutcome::Restored;
    }
    // NeedsRestore 是请求，不是所有权证明。成功发出但尚未读回的写入，
    // 也必须在此补做内容比对；未写过或内容已变，一律隔离。
    if ((item->state != AntiRollItemState::Applied &&
         item->state != AntiRollItemState::NeedsRestore) ||
        (!item->hasApplied && !item->hasAcknowledged)) {
        quarantineItemLocked(&record, item,
                             "本程序从未写入该字段，但内容与记录的原值不符（地址可能已被复用）");
        if (detail) *detail += "内容与记录不符（已隔离，未写入）；";
        return ItemOutcome::Quarantined;
    }
    const bool matchesWrite =
        (item->hasApplied && closeFloat(currentValue, item->applied, 1e-4f)) ||
        (item->hasAcknowledged && closeFloat(currentValue, item->acknowledged, 1e-4f));
    if (!matchesWrite) {
        quarantineItemLocked(&record, item,
                             "字段内容既不是原值也不是本程序写入的值（可能已被他方修改或地址复用）");
        if (detail) *detail += "字段内容不符（已隔离，未写入）；";
        return ItemOutcome::Quarantined;
    }
    if (!writeFloatVerified(memory, item->address, item->original)) {
        if (detail) {
            *detail += fmt("写回 0x%llX（%.3f → %.3f）失败或回读不一致；",
                           (unsigned long long)item->address, (double)currentValue,
                           (double)item->original);
        }
        return ItemOutcome::Retry;
    }
    item->state = AntiRollItemState::Restored;
    return ItemOutcome::Restored;
}

void AntiRollBackupManager::restoreOtherVehicles(ProcessMemory& memory, uint64_t currentTruck,
                                                 size_t* remaining, size_t* quarantined,
                                                 std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t quarantineBefore = quarantineTotal_;
    std::string detail;
    size_t retryLeft = 0;
    for (size_t index = 0; index < vehicles_.size();) {
        AntiRollRecord& record = vehicles_[index];
        if (record.truckObject == currentTruck) {
            ++index;
            continue;
        }
        bool retryNeeded = false;
        for (AntiRollItem* item : {&record.com, &record.truckCvar, &record.trailerCvar}) {
            if (!item->active()) continue;
            const ItemOutcome outcome =
                restoreItemLocked(memory, record, item, item == &record.com, &detail);
            if (outcome == ItemOutcome::Retry) retryNeeded = true;
        }
        if (retryNeeded) ++retryLeft;
        if (!record.com.active() && !record.truckCvar.active() && !record.trailerCvar.active()) {
            vehicles_.erase(vehicles_.begin() + (std::ptrdiff_t)index);
        } else {
            ++index;
        }
    }
    if (remaining) *remaining = retryLeft;
    // 本轮隔离数用单调计数差值判断（诊断队列有长度上限，长度差在队列满时会误判为 0）
    if (quarantined) *quarantined = (size_t)(quarantineTotal_ - quarantineBefore);
    if (error) {
        if (detail.empty()) error->clear();
        else *error = detail;
    }
}

AntiRollBackupManager::RestoreResult AntiRollBackupManager::restoreAll(ProcessMemory& memory,
                                                                       std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t quarantineBefore = quarantineTotal_;
    bool hadWork = false;
    bool retryNeeded = false;
    std::string detail;
    for (size_t index = 0; index < vehicles_.size();) {
        AntiRollRecord& record = vehicles_[index];
        for (AntiRollItem* item : {&record.com, &record.truckCvar, &record.trailerCvar}) {
            if (!item->active()) continue;
            hadWork = true;
            const ItemOutcome outcome =
                restoreItemLocked(memory, record, item, item == &record.com, &detail);
            if (outcome == ItemOutcome::Retry) retryNeeded = true;
        }
        if (!record.com.active() && !record.truckCvar.active() && !record.trailerCvar.active()) {
            vehicles_.erase(vehicles_.begin() + (std::ptrdiff_t)index);
        } else {
            ++index;
        }
    }
    for (AntiRollItem* item : {&cvar_.truckCvar, &cvar_.trailerCvar}) {
        if (!item->active()) continue;
        hadWork = true;
        const ItemOutcome outcome = restoreItemLocked(memory, cvar_, item, false, &detail);
        if (outcome == ItemOutcome::Retry) retryNeeded = true;
    }
    // 单调计数差值：诊断队列满（上限 8）时长度差会误判为 0，从而把「未恢复且已隔离」
    // 谎报成 Restored。
    const uint64_t newlyQuarantined = quarantineTotal_ - quarantineBefore;
    if (error) {
        if (detail.empty()) error->clear();
        else *error = detail;
    }
    if (retryNeeded) return RestoreResult::Pending;
    // 有项无法确认身份/内容 → 已隔离；不得冒充恢复成功
    if (newlyQuarantined > 0) return RestoreResult::Unverified;
    if (!hadWork) return RestoreResult::NothingToDo;
    return RestoreResult::Restored;
}

AntiRollBackupManager::RestoreResult AntiRollBackupManager::restoreTruck(ProcessMemory& memory,
                                                                       uint64_t truckObject,
                                                                       std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t quarantineBefore = quarantineTotal_;
    bool hadWork = false;
    bool retryNeeded = false;
    std::string detail;
    for (size_t index = 0; index < vehicles_.size();) {
        AntiRollRecord& record = vehicles_[index];
        if (record.truckObject != truckObject) {
            ++index;
            continue;
        }
        for (AntiRollItem* item : {&record.com, &record.truckCvar, &record.trailerCvar}) {
            if (!item->active()) continue;
            hadWork = true;
            const ItemOutcome outcome =
                restoreItemLocked(memory, record, item, item == &record.com, &detail);
            if (outcome == ItemOutcome::Retry) retryNeeded = true;
        }
        if (!record.com.active() && !record.truckCvar.active() && !record.trailerCvar.active()) {
            vehicles_.erase(vehicles_.begin() + (std::ptrdiff_t)index);
        } else {
            ++index;
        }
    }
    const uint64_t newlyQuarantined = quarantineTotal_ - quarantineBefore;
    if (error) {
        if (detail.empty()) error->clear();
        else *error = detail;
    }
    if (retryNeeded) return RestoreResult::Pending;
    if (newlyQuarantined > 0) return RestoreResult::Unverified;
    if (!hadWork) return RestoreResult::NothingToDo;
    return RestoreResult::Restored;
}

bool AntiRollBackupManager::hasPendingRestore() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AntiRollRecord& record : vehicles_) {
        if (record.com.active() || record.truckCvar.active() || record.trailerCvar.active()) {
            return true;
        }
    }
    return cvar_.truckCvar.active() || cvar_.trailerCvar.active();
}

bool AntiRollBackupManager::hasRetryableRestore() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AntiRollRecord& record : vehicles_) {
        for (const AntiRollItem* item : {&record.com, &record.truckCvar, &record.trailerCvar}) {
            if (item->state == AntiRollItemState::Captured ||
                item->state == AntiRollItemState::Applied ||
                item->state == AntiRollItemState::NeedsRestore) {
                return true;
            }
        }
    }
    for (const AntiRollItem* item : {&cvar_.truckCvar, &cvar_.trailerCvar}) {
        if (item->state == AntiRollItemState::Captured ||
            item->state == AntiRollItemState::Applied ||
            item->state == AntiRollItemState::NeedsRestore) {
            return true;
        }
    }
    return false;
}

bool AntiRollBackupManager::cvarCaptured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cvar_.truckCvar.active() && cvar_.trailerCvar.active();
}

bool AntiRollBackupManager::hasRecordFor(uint64_t truckObject) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AntiRollRecord& record : vehicles_) {
        if (record.truckObject == truckObject && record.com.active()) return true;
    }
    return false;
}

bool AntiRollBackupManager::identityMatchesFor(uint64_t truckObject,
                                               const VehicleIdentity& identity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AntiRollRecord& record : vehicles_) {
        if (record.truckObject == truckObject && record.com.active()) {
            return record.identityKnown && record.identity == identity;
        }
    }
    return false;
}

size_t AntiRollBackupManager::pendingVehicleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vehicles_.size();
}

std::vector<AntiRollRecord> AntiRollBackupManager::records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vehicles_;
}

std::vector<AntiRollQuarantine> AntiRollBackupManager::quarantined() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quarantine_;
}

uint64_t AntiRollBackupManager::quarantineTotal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quarantineTotal_;
}

// 从车辆对象读取身份指纹（底盘描述符 / 油箱容量位模式 / 磨损数组元素个数）。
bool readVehicleIdentity(const ProcessMemory& memory, uint64_t truckObject,
                         VehicleIdentity* identity) {
    if (!truckObject) return false;
    uint64_t chassisDescriptor = 0;
    uint64_t wearCount = 0;
    float capacity = 0.0f;
    if (!memory.read(truckObject + kTruckChassisDescriptor, &chassisDescriptor,
                     sizeof(chassisDescriptor))) {
        return false;
    }
    if (!memory.read(truckObject + kTruckFuelCapacity, &capacity, sizeof(capacity)) ||
        !std::isfinite(capacity) || capacity < 0.0f) {
        return false;
    }
    if (!memory.read(truckObject + kTruckWheelWearCount, &wearCount, sizeof(wearCount)) ||
        wearCount > kMaxWheelFields) {
        return false;
    }
    VehicleIdentity found;
    found.chassisDescriptor = chassisDescriptor;
    ::memcpy(&found.fuelCapacityBits, &capacity, sizeof(found.fuelCapacityBits));
    found.wheelWearCountBits = (uint32_t)wearCount;
    if (identity) *identity = found;
    return true;
}

bool VehicleLocker::bind(ProcessMemory* memory, DWORD pid, std::string* error) {
    stop();
    memory_ = memory;
    pid_ = pid;
    if (!memory_ || !memory_->isOpen()) {
        if (error) *error = "游戏进程尚未附加";
        return false;
    }
    if (!locateRootGlobal(error)) {
        memory_ = nullptr;
        pid_ = 0;
        return false;
    }
    // 绑定到新的地址空间：先隔离旧代次记录，之后才允许对本进程做恢复/写入。
    // （与 bindForTesting 共用同一实现；生产入口不含任何「测试独有」的安全逻辑。）
    beginNewProcessGeneration();
    setStatus("已绑定 1.61 固定车辆指针（无数值扫描）");
    return true;
}

bool VehicleLocker::locateRootGlobal(std::string* error) {
    ModuleInfo module;
    if (!mainModule(pid_, &module) || !module.base ||
        module.size < kRootInstructionRva + sizeof(kRootInstruction)) {
        if (error) *error = "无法取得 eurotrucks2.exe 主模块";
        return false;
    }
    moduleBase_ = 0;
    std::array<uint8_t, sizeof(kRootInstruction)> actual{};
    if (!memory_->read(module.base + kRootInstructionRva, actual.data(), actual.size()) ||
        ::memcmp(actual.data(), kRootInstruction, actual.size()) != 0) {
        if (error) *error = "游戏主程序不是已验证的 ETS2 1.61.1.1，已拒绝使用固定地址";
        return false;
    }
    // 只有指令校验通过（确认版本）后才记录模块基址，供 cvar 固定 RVA 使用。
    moduleBase_ = module.base;
    int32_t displacement = 0;
    ::memcpy(&displacement, actual.data() + 3, sizeof(displacement));
    rootGlobal_ = module.base + kRootInstructionRva + 7 + static_cast<int64_t>(displacement);
    return isCanonicalPointer(rootGlobal_);
}

// 只走 4 级指针链并填充防侧翻相关地址；不做遥测校验。
// 供「防侧翻单独启用」与「关闭还原」路径使用：这两条路径不应被油量遥测偶发不一致阻塞。
bool VehicleLocker::resolveChainOnly(VehicleAddresses* addresses, std::string* error) const {
    if (!memory_ || !memory_->isOpen() || !rootGlobal_) {
        if (error) *error = "固定车辆指针尚未绑定";
        return false;
    }
    uint64_t root = 0, driver = 0, context = 0, truck = 0;
    if (!readPointer(*memory_, rootGlobal_, &root) ||
        !readPointer(*memory_, root + kRootToDriver, &driver) ||
        !readPointer(*memory_, driver + kDriverToContext, &context) ||
        !readPointer(*memory_, context + kContextToTruck, &truck)) {
        if (error) *error = "当前没有可用车辆（请进入驾驶界面）";
        return false;
    }
    VehicleAddresses found;
    found.rootGlobal = rootGlobal_;
    found.context = context;
    found.truck = truck;
    found.truckCenterOfMassY = truck + kTruckCenterOfMassY;
    if (moduleBase_) {
        found.truckStabilityCvar = moduleBase_ + kCvarTruckStabilityFloat;
        found.trailerStabilityCvar = moduleBase_ + kCvarTrailerStabilityFloat;
    }
    if (addresses) *addresses = std::move(found);
    return true;
}

bool VehicleLocker::resolveAndValidate(VehicleAddresses* addresses,
                                       TelemetrySnapshot* telemetry,
                                       std::string* error, bool requireTelemetry) const {
    VehicleAddresses found;
    if (!resolveChainOnly(&found, error)) return false;
    const uint64_t truck = found.truck;
    const uint64_t context = found.context;

    TelemetrySnapshot live;
    std::string telemetryError;
    if (requireTelemetry && !readTelemetry(&live, &telemetryError)) {
        if (error) *error = "安全校验需要遥测数据：" + telemetryError;
        return false;
    }

    float capacity = 0.0f, ratio = 0.0f, correction = 0.0f;
    std::array<float, 8> wearParts{};
    if (!readFloat(*memory_, truck + kTruckFuelCapacity, &capacity) ||
        !readFloat(*memory_, truck + kTruckFuelRatio, &ratio) ||
        !readFloat(*memory_, context + kContextFuelCorrection, &correction) ||
        !readFloat(*memory_, truck + kTruckEngineWear, &wearParts[0]) ||
        !readFloat(*memory_, truck + kTruckEnginePermanent, &wearParts[1]) ||
        !readFloat(*memory_, truck + kTruckTransmissionWear, &wearParts[2]) ||
        !readFloat(*memory_, truck + kTruckTransmissionPermanent, &wearParts[3]) ||
        !readFloat(*memory_, truck + kTruckCabinWear, &wearParts[4]) ||
        !readFloat(*memory_, truck + kTruckCabinPermanent, &wearParts[5]) ||
        !readFloat(*memory_, truck + kTruckChassisWear, &wearParts[6]) ||
        !readFloat(*memory_, truck + kTruckChassisPermanent, &wearParts[7])) {
        if (error) *error = "读取固定车辆字段失败";
        return false;
    }
    if (capacity <= 0.0f || capacity > 10000.0f || ratio < -0.01f || ratio > 1.10f ||
        correction < -1.10f || correction > 1.10f) {
        if (error) *error = "固定车辆字段范围异常，已拒绝写入";
        return false;
    }
    for (float value : wearParts) {
        if (value < -0.001f || value > 1.001f) {
            if (error) *error = "固定损伤字段范围异常，已拒绝写入";
            return false;
        }
    }

    found.fuel = truck + kTruckFuelRatio;
    found.fuelCorrection = context + kContextFuelCorrection;
    found.damageFields = {
        truck + kTruckEngineWear,        truck + kTruckEnginePermanent,
        truck + kTruckTransmissionWear,  truck + kTruckTransmissionPermanent,
        truck + kTruckCabinWear,         truck + kTruckCabinPermanent,
        truck + kTruckChassisWear,       truck + kTruckChassisPermanent,
    };

    float wheelWear = 0.0f, wheelPermanent = 0.0f;
    if (!readWheelGroup(*memory_, truck, kTruckWheelWearArray, kTruckWheelWearCount,
                        kTruckWheelWearCache, &wheelWear, &found.damageFields) ||
        !readWheelGroup(*memory_, truck, kTruckWheelPermanentArray,
                        kTruckWheelPermanentCount, kTruckWheelPermanentCache,
                        &wheelPermanent, &found.damageFields)) {
        if (error) *error = "读取车轮损伤数组失败，已拒绝写入";
        return false;
    }

    const float expectedFuel = capacity * (ratio + correction);
    std::array<float, 5> expectedWear = {
        clampWear(wearParts[0] + wearParts[1]),
        clampWear(wearParts[2] + wearParts[3]),
        clampWear(wearParts[4] + wearParts[5]),
        clampWear(wearParts[6] + wearParts[7]),
        clampWear(wheelWear + wheelPermanent),
    };
    if (requireTelemetry) {
        const bool fuelAlreadyLocked = fuelEnabled() && ratio >= 0.999f;
        const float fuelTolerance = (std::max)(2.0f, live.fuelCapacity * 0.005f);
        if (!closeFloat(capacity, live.fuelCapacity, 1.0f) ||
            (!fuelAlreadyLocked && !closeFloat(expectedFuel, live.fuel, fuelTolerance))) {
            if (error) {
                *error = fmt("固定油量字段未通过遥测校验（内存 %.2f/%.2f L，遥测 %.2f/%.2f L）",
                             expectedFuel, capacity, live.fuel, live.fuelCapacity);
            }
            return false;
        }
        for (size_t index = 0; index < expectedWear.size(); ++index) {
            const bool alreadyLocked = damageEnabled() && expectedWear[index] <= 0.002f;
            if (!alreadyLocked && !closeFloat(expectedWear[index], live.wear[index], 0.002f)) {
                if (error) {
                    *error = fmt("固定损伤字段第 %llu 项未通过遥测校验（内存 %.6f，遥测 %.6f）",
                                 (unsigned long long)(index + 1), expectedWear[index],
                                 live.wear[index]);
                }
                return false;
            }
        }
    }

    if (addresses) *addresses = std::move(found);
    if (telemetry) *telemetry = live;
    return true;
}

bool VehicleLocker::probe(VehicleAddresses* addresses, TelemetrySnapshot* telemetry,
                          std::string* error) {
    return resolveAndValidate(addresses, telemetry, error);
}

// 事务式写入：逐项「读取现值 → 写入 → 立即回读验证」；每一步都先把**确实写入并验证成功**
// 的字段记入恢复凭据（绝不用占位值覆盖已有凭据）。任一项失败则回滚本轮已写入项
// （先全部写回再统一回读验证）；回滚未成功的字段记为 NeedsRestore 并保留真实写入值，
// 交由后续周期安全重试 —— 绝不允许「报告失败却留下部分修改而没有恢复任务」。
bool VehicleLocker::applyTransactional(const VehicleAddresses& addresses, bool fuel, bool damage,
                                      bool antiRoll, float factor, std::string* error) {
    struct Field {
        uint64_t    address = 0;
        float       target = 0.0f;
        float       rollback = 0.0f;
        bool        written = false;
        const char* label = "";
    };
    std::vector<Field> fields;
    const float one = 1.0f;
    const float zero = 0.0f;

    if (fuel) {
        fields.push_back({addresses.fuel, one, zero, false, "油量"});
        fields.push_back({addresses.fuelCorrection, zero, zero, false, "油量修正"});
    }
    if (damage) {
        for (uint64_t address : addresses.damageFields) {
            fields.push_back({address, zero, zero, false, "损伤"});
        }
    }
    float clampedFactor = factor;
    if (!(clampedFactor >= 1.0f)) clampedFactor = 1.0f;
    if (clampedFactor > 6.0f) clampedFactor = 6.0f;
    if (antiRoll) {
        if (addresses.truckCenterOfMassY) {
            float defaultHeight = 0.75f;
            uint64_t chassisDesc = 0;
            if (readPointer(*memory_, addresses.truck + kTruckChassisDescriptor, &chassisDesc) &&
                chassisDesc) {
                float readHeight = 0.0f;
                if (readFloat(*memory_, chassisDesc + kChassisDefaultCoMHeight, &readHeight) &&
                    readHeight > 0.05f && readHeight < 5.0f) {
                    defaultHeight = readHeight;
                }
            }
            float targetCoMY = -(clampedFactor - 1.0f) * defaultHeight;
            if (targetCoMY < -6.0f) targetCoMY = -6.0f;
            if (targetCoMY > -0.2f) targetCoMY = -0.2f;
            fields.push_back({addresses.truckCenterOfMassY, targetCoMY, 0.0f, false, "重心"});
        }
        // 稳定性 cvar：仅在成功快照过原始值后才写（宁缺毋滥）
        if (antiRollBackup_.cvarCaptured() && addresses.truckStabilityCvar &&
            addresses.trailerStabilityCvar) {
            fields.push_back(
                {addresses.truckStabilityCvar, clampedFactor, 0.0f, false, "车头稳定性"});
            fields.push_back(
                {addresses.trailerStabilityCvar, clampedFactor, 0.0f, false, "挂车稳定性"});
        }
    }

    // 回滚：把本轮写入项全部写回「写入前的值」，再统一回读验证并逐项记录结果。
    auto rollback = [&](size_t count) {
        if (rollbackHookForTesting_) rollbackHookForTesting_();  // 测试同步点（注入回滚失败）
        for (size_t i = 0; i < count && i < fields.size(); ++i) {
            Field& field = fields[i];
            if (!field.written) continue;
            if (memory_->write(field.address, &field.rollback, sizeof(field.rollback))) {
                antiRollBackup_.noteFieldWriteSucceeded(field.address, field.rollback);
            }
        }
        size_t failed = 0;
        for (size_t i = 0; i < count && i < fields.size(); ++i) {
            Field& field = fields[i];
            if (!field.written) continue;
            field.written = false;
            float actual = 0.0f;
            const bool ok = readFloat(*memory_, field.address, &actual) &&
                            closeFloat(actual, field.rollback, 1e-4f);
            if (ok) {
                antiRollBackup_.noteFieldValueVerified(field.address, actual);
            } else {
                ++failed;
                // 未知内容不冒充目标值；保留本字段已有的成功写入/确认凭据。
                antiRollBackup_.markFieldPendingRestore(field.address);
            }
        }
        return failed;
    };

    // 回滚结果说明（注意：std::string 绝不能直接传给 printf 风格的 %s）
    auto rollbackNote = [](size_t failed) {
        return failed ? fmt("，且回滚有 %d 项未成功（已保留真实写入值，稍后重试）", (int)failed)
                      : std::string("（本轮已写入项已回滚）");
    };

    for (size_t i = 0; i < fields.size(); ++i) {
        Field& field = fields[i];
        // 写入前记录当前值：事务失败时用它精确回滚（不依赖猜测的默认值）
        float before = 0.0f;
        if (!readFloat(*memory_, field.address, &before)) {
            const std::string note = rollbackNote(rollback(i));
            if (error) *error = fmt("读取%s字段当前值失败", field.label) + note;
            return false;
        }
        if (antiRoll && field.address == addresses.truckCenterOfMassY &&
            !plausibleCenterOfMass(before)) {
            const std::string note = rollbackNote(rollback(i));
            if (error) *error = "当前车辆重心偏移异常，拒绝继续写入" + note;
            return false;
        }
        field.rollback = before;
        field.written = true;  // 已发出写入：回滚时一并按现值核对
        if (!memory_->write(field.address, &field.target, sizeof(field.target))) {
            const std::string note = rollbackNote(rollback(i + 1));
            if (error) *error = fmt("写入%s字段失败", field.label) + note;
            return false;
        }
        antiRollBackup_.noteFieldWriteSucceeded(field.address, field.target);
        // 立即回读验证本字段：成功才把写入值记入凭据（失败则连同本字段一起回滚）
        float actual = 0.0f;
        if (!readFloat(*memory_, field.address, &actual) ||
            !closeFloat(actual, field.target, 1e-4f)) {
            const std::string note = rollbackNote(rollback(i + 1));
            if (error) {
                *error = fmt("回读校验失败：%s 字段期望 %.4f 实际 %.4f", field.label,
                             (double)field.target, (double)actual) +
                         note;
            }
            return false;
        }
        antiRollBackup_.noteFieldValueVerified(field.address, actual);
    }
    // 全部写完后做一次整体一致性复验：逐字段验证只能保证「写入当时」正确，
    // 字段地址重叠/别名时后续写入会破坏先前字段 —— 必须再查一遍，任何不一致都回滚。
    for (const Field& field : fields) {
        float actual = 0.0f;
        if (!readFloat(*memory_, field.address, &actual) ||
            !closeFloat(actual, field.target, 1e-4f)) {
            const std::string note = rollbackNote(rollback(fields.size()));
            if (error) {
                *error = fmt("整体复验失败：%s 字段期望 %.4f 实际 %.4f", field.label,
                             (double)field.target, (double)actual) +
                         note;
            }
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

// 写入 + 回读验证（回滚专用；失败返回 false，调用方据此保留恢复记录）
bool VehicleLocker::writeFloatVerifiedSafe(uint64_t address, float value) {
    if (!memory_ || !memory_->isOpen() || !address) return false;
    if (!memory_->write(address, &value, sizeof(value))) return false;
    float readBack = 0.0f;
    if (!memory_->read(address, &readBack, sizeof(readBack))) return false;
    return closeFloat(readBack, value, 1e-6f);
}

bool VehicleLocker::setFuelEnabled(bool enabled, std::string* error) {
    const VehicleCommandState current = commands();
    applyCommand(enabled, current.damage, current.antiRoll, current.antiRollFactor);
    if (!enabled) {
        setStatus(comboStatusText());
        return true;
    }
    if (!memory_ || !memory_->isOpen()) {
        applyCommand(false, current.damage, current.antiRoll, current.antiRollFactor);
        if (error) *error = "游戏进程尚未附加";
        return false;
    }
    // 所有写入（含防侧翻）都走同一个周期入口：先复核身份/建立备份，再事务式写入 ——
    // 油量/无损入口不得绕过防侧翻备份。
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error)) {
        applyCommand(false, current.damage, current.antiRoll, current.antiRollFactor);
        return false;
    }
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    const CycleResult result = runCycleLocked(addresses, true, error);
    if (result.action != CycleAction::Applied) {
        applyCommand(false, current.damage, current.antiRoll, current.antiRollFactor);
        if (error && error->empty()) *error = result.note;
        setStatus("无限油量启用失败：" + (error ? *error : std::string("写入被拒绝")));
        scheduleRestoreIfNeeded();  // 失败可能留下部分修改：必须确保后台周期会重试恢复
        return false;
    }
    setStatus(fmt("无限油量已锁定（%.1f / %.1f L）", telemetry.fuel, telemetry.fuelCapacity));
    ensureThread();
    return true;
}

bool VehicleLocker::setDamageEnabled(bool enabled, std::string* error) {
    const VehicleCommandState current = commands();
    applyCommand(current.fuel, enabled, current.antiRoll, current.antiRollFactor);
    if (!enabled) {
        setStatus(comboStatusText());
        return true;
    }
    if (!memory_ || !memory_->isOpen()) {
        applyCommand(current.fuel, false, current.antiRoll, current.antiRollFactor);
        if (error) *error = "游戏进程尚未附加";
        return false;
    }
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error)) {
        applyCommand(current.fuel, false, current.antiRoll, current.antiRollFactor);
        return false;
    }
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    const CycleResult result = runCycleLocked(addresses, true, error);
    if (result.action != CycleAction::Applied) {
        applyCommand(current.fuel, false, current.antiRoll, current.antiRollFactor);
        if (error && error->empty()) *error = result.note;
        setStatus("车辆无损启用失败：" + (error ? *error : std::string("写入被拒绝")));
        scheduleRestoreIfNeeded();  // 失败可能留下部分修改：必须确保后台周期会重试恢复
        return false;
    }
    setStatus("车辆五项损伤已清零并锁定");
    ensureThread();
    return true;
}

bool VehicleLocker::setAntiRollEnabled(bool enabled, float factor, std::string* error) {
    // 入参收敛到安全区间
    if (!(factor >= 1.0f)) factor = 1.0f;
    if (factor > 6.0f) factor = 6.0f;
    const VehicleCommandState current = commands();
    applyCommand(current.fuel, current.damage, enabled, factor);

    if (!enabled) {
        // 仅更新命令状态：还原由周期统一处理（命令状态是唯一事实来源，
        // 不会像「独立 pending 标志」那样被旧周期的过期快照覆盖或丢弃）。
        setStatus(antiRollBackup_.hasPendingRestore()
                      ? "防侧翻已关闭，正在还原车辆物理参数…"
                      : comboStatusText());
        ensureThread();
        return true;
    }

    if (!memory_ || !memory_->isOpen()) {
        applyCommand(current.fuel, current.damage, false, factor);
        if (error) *error = "游戏进程尚未附加";
        return false;
    }
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error, /*requireTelemetry=*/false)) {
        applyCommand(current.fuel, current.damage, false, factor);
        return false;
    }
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    const CycleResult result = runCycleLocked(addresses, false, error);
    if (result.action != CycleAction::Applied) {
        applyCommand(current.fuel, current.damage, false, factor);
        if (error && error->empty()) *error = result.note;
        setStatus("防侧翻启用失败：" + (error ? *error : std::string("写入被拒绝")));
        scheduleRestoreIfNeeded();  // 首次启用失败也可能留下修改：必须调度恢复周期
        return false;
    }
    float originalY = 0.0f;
    for (const AntiRollRecord& record : antiRollBackup_.records()) {
        if (record.truckObject == addresses.truck && record.identityKnown) {
            originalY = record.com.original;
            break;
        }
    }
    setStatus(fmt("防侧翻/不倒翁已锁定（强度 %.1fx，车辆 %s 原重心 Y=%.2f 已快照）", factor,
                  hexAddr(addresses.truck).c_str(), (double)originalY));
    ensureThread();
    return true;
}
// ===========================================================================
// 统一命令状态 / 单写者周期
// ===========================================================================
void VehicleLocker::applyCommand(bool fuel, bool damage, bool antiRoll, float factor) {
    if (!(factor >= 1.0f)) factor = 1.0f;
    if (factor > 6.0f) factor = 6.0f;
    std::lock_guard<std::mutex> lock(commandMutex_);
    command_.fuel = fuel;
    command_.damage = damage;
    command_.antiRoll = antiRoll;
    command_.antiRollFactor = factor;
    ++command_.sequence;  // 每次变化都推进序号：旧周期据此丢弃过期任务
}

VehicleCommandState VehicleLocker::commands() const {
    std::lock_guard<std::mutex> lock(commandMutex_);
    return command_;
}

uint64_t VehicleLocker::commandSequence() const {
    std::lock_guard<std::mutex> lock(commandMutex_);
    return command_.sequence;
}

bool VehicleLocker::fuelEnabled() const { return commands().fuel; }
bool VehicleLocker::damageEnabled() const { return commands().damage; }
bool VehicleLocker::antiRollEnabled() const { return commands().antiRoll; }
float VehicleLocker::antiRollFactor() const { return commands().antiRollFactor; }

void VehicleLocker::setWriteGuard(WriteGuard guard) {
    // 加锁安装：worker 线程可能正在运行并读取同一个 std::function（运行中替换守卫）
    std::lock_guard<std::mutex> lock(guardMutex_);
    guard_ = std::move(guard);
}

void VehicleLocker::setGuardIntervalMs(uint64_t milliseconds) {
    guardIntervalMs_ = milliseconds;
}

void VehicleLocker::setCycleHookAfterSnapshot(std::function<void()> hook) {
    cycleHookAfterSnapshot_ = std::move(hook);
}

void VehicleLocker::setRollbackHookForTesting(std::function<void()> hook) {
    rollbackHookForTesting_ = std::move(hook);
}

void VehicleLocker::beginNewProcessGeneration() {
    // 生产 bind() 与测试绑定共用：绑定实际地址空间后、任何新进程写入/恢复之前，
    // 旧代次记录一律隔离（不向新进程重放旧地址）。
    antiRollBackup_.beginProcessGeneration(++nextGeneration_);
}

void VehicleLocker::scheduleRestoreIfNeeded() {
    // 事务失败后若留下待恢复项，必须确保有后台周期去重试（否则无人恢复）
    if (antiRollBackup_.hasPendingRestore()) ensureThread();
}

bool VehicleLocker::bindForTesting(ProcessMemory* memory, uint64_t rootGlobal) {
    stop();
    memory_ = memory;
    pid_ = 0;
    rootGlobal_ = rootGlobal;
    moduleBase_ = 0;
    if (!memory_ || !memory_->isOpen()) return false;
    beginNewProcessGeneration();  // 与生产 bind() 完全相同的生命周期实现
    return true;
}

void VehicleLocker::setCommandsForTesting(bool fuel, bool damage, bool antiRoll, float factor) {
    applyCommand(fuel, damage, antiRoll, factor);
}

std::string VehicleLocker::guardReason() {
    const uint64_t now = ::GetTickCount64();
    const uint64_t sequence = commandSequence();
    const bool sequenceChanged = sequence != guardCheckedSequence_;
    if (!sequenceChanged && now - guardCheckedAt_ < guardIntervalMs_) return std::string();
    guardCheckedAt_ = now;
    guardCheckedSequence_ = sequence;
    // 加锁复制守卫后在锁外调用：避免与运行中的 setWriteGuard 竞争，也不持锁执行外部代码
    WriteGuard guardSnapshot;
    {
        std::lock_guard<std::mutex> lock(guardMutex_);
        guardSnapshot = guard_;
    }
    if (!guardSnapshot) return std::string();
    return guardSnapshot();
}

// 单周期主逻辑（worker 线程与界面线程共用，调用方必须持有 writeMutex_）。
//   1) 命令快照 → 2) 写入闸门（联机保护）→ 3) 换车/备份 → 4) 事务式写入或还原
//   5) 写入前重新确认命令未变化（丢弃过期任务，避免旧状态覆盖新的关闭请求）
VehicleLocker::CycleResult VehicleLocker::runCycleLocked(const VehicleAddresses& addresses,
                                                        bool requireTelemetry,
                                                        std::string* error) {
    CycleResult result;
    if (!memory_ || !memory_->isOpen()) {
        result.action = CycleAction::Idle;
        result.note = "游戏进程尚未绑定";
        return result;
    }
    const uint64_t sequence = commandSequence();
    VehicleCommandState state;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        state = command_;
    }
    // 测试注入点：模拟「取快照之后用户关闭了功能」
    if (cycleHookAfterSnapshot_) cycleHookAfterSnapshot_();

    // 所有恢复/清除分支也受序号约束，不能只在最终目标写入前检查。
    if (commandSequence() != state.sequence) {
        return CycleResult{CycleAction::SkippedStale, "状态在准备期间已变化，本轮不写入或恢复"};
    }

    // ---- 写入闸门：真正执行写入前复查联机状态 ----
    const std::string blocked = guardReason();
    if (commandSequence() != state.sequence) {
        return CycleResult{CycleAction::SkippedStale, "闸门检查期间命令已变化，本轮不写入或恢复"};
    }
    if (!blocked.empty()) {
        if (state.fuel || state.damage || state.antiRoll) {
            applyCommand(false, false, false, state.antiRollFactor);
            antiRollBackup_.markNeedsRestore(addresses.truck);
        }
        std::string restoreError;
        const AntiRollBackupManager::RestoreResult restored =
            antiRollBackup_.restoreAll(*memory_, &restoreError);
        result.action = CycleAction::Blocked;
        result.note = "写入闸门拦截：" + blocked +
                      (restored == AntiRollBackupManager::RestoreResult::Restored
                           ? "；已恢复防侧翻原值"
                           : (restored == AntiRollBackupManager::RestoreResult::Pending
                                  ? "；还原暂缓（记录已保留）"
                                  : (restored == AntiRollBackupManager::RestoreResult::Unverified
                                         ? "；有项无法确认身份，已隔离"
                                         : std::string())));
        if (error) *error = result.note;
        return result;
    }

    const bool antiRollActive = state.antiRoll;
    // 强度 <= 1.01 视为「等效原厂」：不写重心/不写 cvar，改为把本车已写入项还原成原值
    const bool antiRollWrite = antiRollActive && state.antiRollFactor > 1.01f;
    std::string antiRollWarning;
    if (antiRollActive && !antiRollWrite) {
        std::string restoreError;
        const AntiRollBackupManager::RestoreResult restored =
            antiRollBackup_.restoreTruck(*memory_, addresses.truck, &restoreError);
        switch (restored) {
            case AntiRollBackupManager::RestoreResult::Restored:
            case AntiRollBackupManager::RestoreResult::NothingToDo:
                result.action = CycleAction::Applied;
                result.note = "防侧翻强度 1.0（等效原厂）：本车已还原为原厂值";
                break;
            case AntiRollBackupManager::RestoreResult::Pending:
                result.action = CycleAction::RestorePending;
                result.note = "强度 1.0 还原暂缓：" + restoreError + "（记录已保留，稍后重试）";
                break;
            case AntiRollBackupManager::RestoreResult::Unverified:
                result.action = CycleAction::Unverified;
                result.note = "强度 1.0 还原时发现无法确认的字段，已隔离：" + restoreError;
                break;
        }
        // 油量/无损仍按命令继续写入
        if (!state.fuel && !state.damage) {
            if (error) *error = result.note;
            return result;
        }
    }
    if (antiRollActive) {
        // ---- 换车/读档：先复核并还原其它车辆，再为当前车辆建立独立备份 ----
        size_t remaining = 0;
        size_t newlyQuarantined = 0;
        std::string restoreError;
        antiRollBackup_.restoreOtherVehicles(*memory_, addresses.truck, &remaining,
                                             &newlyQuarantined, &restoreError);
        if (antiRollBackup_.hasRecordFor(addresses.truck)) {
            // 已启用期间每周期复核车辆身份：地址若被别的对象复用，绝不继续写入
            VehicleIdentity identity;
            if (!readVehicleIdentity(*memory_, addresses.truck, &identity)) {
                result.action = CycleAction::Failed;
                result.note = "暂时无法确认车辆身份（对象不可读），本轮不写入；恢复记录保留";
                if (error) *error = result.note;
                return result;
            }
            if (!antiRollBackup_.identityMatchesFor(addresses.truck, identity)) {
                antiRollBackup_.markNeedsRestore(addresses.truck);
                std::string quarantineError;
                const AntiRollBackupManager::RestoreResult restored =
                    antiRollBackup_.restoreAll(*memory_, &quarantineError);
                result.action = CycleAction::Unverified;
                result.note = "车辆身份与记录不符（地址可能已被复用）：已隔离并停止写入" +
                              ((restored == AntiRollBackupManager::RestoreResult::Unverified ||
                                restored == AntiRollBackupManager::RestoreResult::Pending)
                                   ? ("；" + quarantineError)
                                   : std::string());
                if (error) *error = result.note;
                return result;
            }
        } else {
            VehicleIdentity identity;
            if (!readVehicleIdentity(*memory_, addresses.truck, &identity)) {
                result.action = CycleAction::Failed;
                result.note = "无法读取车辆身份指纹，已拒绝写入（避免把别的对象当成这辆车）";
                if (error) *error = result.note;
                return result;
            }
            std::string captureError;
            if (!antiRollBackup_.captureCenterOfMass(*memory_, addresses.truck,
                                                     addresses.truckCenterOfMassY, identity,
                                                     &captureError)) {
                result.action = CycleAction::Failed;
                result.note = "无法为当前车辆备份原重心：" + captureError +
                              "；已有恢复记录不会被覆盖";
                if (error) *error = result.note;
                return result;
            }
            std::string cvarError;
            if (!antiRollBackup_.captureCvars(*memory_, addresses.truckStabilityCvar,
                                              addresses.trailerStabilityCvar, &cvarError)) {
                // 读不到 cvar 原值 → 保持未捕获，写入端会跳过 cvar（宁缺毋滥）
            }
        }
        if (remaining > 0 || newlyQuarantined > 0) {
            antiRollWarning = fmt("旧记录：待重试 %d 项，新隔离 %d 项（%s）", (int)remaining,
                                  (int)newlyQuarantined, restoreError.c_str());
        }
    } else if (antiRollBackup_.hasPendingRestore()) {
        // ---- 关闭/停止：按记录逐项写回并读回验证 ----
        std::string restoreError;
        const AntiRollBackupManager::RestoreResult restored =
            antiRollBackup_.restoreAll(*memory_, &restoreError);
        switch (restored) {
            case AntiRollBackupManager::RestoreResult::Restored:
                result.action = CycleAction::Restored;
                result.note = "防侧翻已关闭：重心与稳定性参数已还原为开启前的原值（回读一致）";
                break;
            case AntiRollBackupManager::RestoreResult::Pending:
                result.action = CycleAction::RestorePending;
                result.note = "还原暂缓：" + restoreError + "（记录已保留，稍后自动重试）";
                break;
            case AntiRollBackupManager::RestoreResult::Unverified:
                result.action = CycleAction::Unverified;
                result.note = "有项无法确认身份/内容，已隔离（未写入、不冒充恢复成功）：" +
                              restoreError;
                break;
            case AntiRollBackupManager::RestoreResult::NothingToDo:
                result.action = CycleAction::Idle;
                result.note = "没有需要恢复的数据";
                break;
        }
        return result;
    }

    const bool fuel = state.fuel;
    const bool damage = state.damage;
    if (!fuel && !damage && !antiRollActive) {
        result.action = CycleAction::Idle;
        return result;
    }
    if (requireTelemetry) {
        // 由调用方保证遥测已校验（worker 在 resolveAndValidate 中完成）
    }

    // ---- 写入前重新确认命令未变化：过期任务不得覆盖更新的关闭请求 ----
    if (commandSequence() != sequence) {
        result.action = CycleAction::SkippedStale;
        result.note = "状态在准备期间已变化，本轮不写入（丢弃过期任务）";
        return result;
    }

    std::string writeError;
    if (!applyTransactional(addresses, fuel, damage, antiRollWrite, state.antiRollFactor,
                            &writeError)) {
        // 事务失败：已写入项已回滚（或标记 NeedsRestore 安排重试）
        result.action = CycleAction::Failed;
        result.note = writeError;
        if (error) *error = writeError;
        return result;
    }

    result.action = CycleAction::Applied;
    std::vector<std::string> activeList;
    if (fuel) activeList.push_back("无限油量");
    if (damage) activeList.push_back("车辆无损");
    if (antiRollWrite) {
        activeList.push_back(fmt("防侧翻/不倒翁(%.1fx)", state.antiRollFactor));
    }
    std::string text;
    for (size_t i = 0; i < activeList.size(); ++i) {
        if (i > 0) text += " + ";
        text += activeList[i];
    }
    text += " 锁定中（固定指针）";
    if (!antiRollWarning.empty()) text += "。" + antiRollWarning;
    result.note = text;
    return result;
}

VehicleLocker::CycleResult VehicleLocker::runMaintenanceCycle(const VehicleAddresses& addresses,
                                                             bool requireTelemetry,
                                                             std::string* error) {
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    return runCycleLocked(addresses, requireTelemetry, error);
}

void VehicleLocker::setAntiRollFactor(float factor) {
    if (!(factor >= 1.0f)) factor = 1.0f;
    if (factor > 6.0f) factor = 6.0f;
    const VehicleCommandState state = commands();
    applyCommand(state.fuel, state.damage, state.antiRoll, factor);
}

void VehicleLocker::ensureThread() {
    if (thread_.joinable()) return;
    stopping_.store(false);
    thread_ = std::thread([this]() { run(); });
}

// 单个 worker 迭代（真实共享实现；离线测试直接驱动它）。
// 关键：**链断恢复路径也必须持有 writeMutex_ 并复核最新命令** —— 否则它会绕过
// 「唯一写者」约束，与界面线程正在进行的启用/写入交错，误隔离有效记录或覆盖新写入。
VehicleLocker::CycleResult VehicleLocker::runWorkerIteration() {
    const VehicleCommandState state = commands();
    std::string error;
    if (!state.fuel && !state.damage && !state.antiRoll &&
        !antiRollBackup_.hasPendingRestore()) {
        return CycleResult{CycleAction::Idle, std::string()};
    }
    if (!state.fuel && !state.damage && !state.antiRoll &&
        !antiRollBackup_.hasRetryableRestore()) {
        return CycleResult{CycleAction::Idle, std::string()};
    }

    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    // 只有油量/无损锁定才需要遥测交叉校验；防侧翻单独开启时跳过
    if (!resolveAndValidate(&addresses, &telemetry, &error, state.fuel || state.damage)) {
        // 测试同步点：模拟「worker 准备恢复期间，UI 重新启用/改变功能」
        if (cycleHookAfterSnapshot_) cycleHookAfterSnapshot_();
        // 链断：仍要尝试还原（记录里保存了地址），但必须与界面线程串行化
        std::lock_guard<std::mutex> writeLock(writeMutex_);
        const VehicleCommandState latest = commands();
        if (latest.sequence != state.sequence) {
            return CycleResult{CycleAction::SkippedStale, "链断但命令已变化：按最新命令处理"};
        }
        if (latest.antiRoll) {
            // 持续链断不是过期任务：交给 run() 的有界等待，不能无休眠忙循环。
            return CycleResult{CycleAction::Blocked, "锁定已暂停：" + error};
        }
        if (antiRollBackup_.hasPendingRestore() && memory_ && memory_->isOpen()) {
            std::string restoreError;
            const AntiRollBackupManager::RestoreResult restored =
                antiRollBackup_.restoreAll(*memory_, &restoreError);
            if (restored == AntiRollBackupManager::RestoreResult::Pending) {
                return CycleResult{CycleAction::RestorePending,
                                   "防侧翻还原暂缓：" + restoreError + "（记录已保留，稍后重试）"};
            }
            if (restored == AntiRollBackupManager::RestoreResult::Unverified) {
                return CycleResult{CycleAction::Unverified,
                                   "还原时发现无法确认的字段，已隔离：" + restoreError};
            }
            if (restored == AntiRollBackupManager::RestoreResult::Restored) {
                // 链断但按记录完成了还原：如实报告还原结果（不是"暂停"）
                return CycleResult{CycleAction::Restored,
                                   "指针链暂不可用，但已按记录还原防侧翻原值"};
            }
        }
        return CycleResult{CycleAction::Blocked, "锁定已暂停：" + error};
    }

    CycleResult result = runMaintenanceCycle(addresses, state.fuel || state.damage, &error);
    if (result.action == CycleAction::Failed) scheduleRestoreIfNeeded();
    return result;
}

void VehicleLocker::run() {
    while (!stopping_.load()) {
        const CycleResult result = runWorkerIteration();
        switch (result.action) {
            case CycleAction::Idle:
                setStatus(comboStatusText());
                ::Sleep(100);
                continue;
            case CycleAction::SkippedStale:
                ::Sleep(10); // 命令持续变化时也有界等待；此时已释放 writeMutex_。
                continue;  // 下一轮重新取最新命令，绝不执行过期周期。
            case CycleAction::Restored:
            case CycleAction::Unverified:
                setStatus(result.note);
                ::Sleep(200);
                continue;
            default:
                setStatus(result.note);
                break;
        }
        ::Sleep(250);
    }
}

void VehicleLocker::stop() {
    applyCommand(false, false, false, commands().antiRollFactor);
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    // 唯一写者：join 之后此处安全地补一次还原（按记录自身地址与身份校验）
    bool restorePendingLeft = false;
    {
        std::lock_guard<std::mutex> writeLock(writeMutex_);
        if (memory_ && memory_->isOpen() && antiRollBackup_.hasPendingRestore()) {
            std::string error;
            const AntiRollBackupManager::RestoreResult result =
                antiRollBackup_.restoreAll(*memory_, &error);
            if (result == AntiRollBackupManager::RestoreResult::Pending) {
                restorePendingLeft = true;
                setStatus("防侧翻：停止时还原未完成，恢复记录已保留（" + error + "）");
            } else if (result == AntiRollBackupManager::RestoreResult::Unverified) {
                restorePendingLeft = true;
                setStatus("防侧翻：有项无法确认身份/内容，已隔离（未写入）：" + error);
            }
        }
    }
    memory_ = nullptr;
    pid_ = 0;
    rootGlobal_ = 0;
    moduleBase_ = 0;
    if (!restorePendingLeft) setStatus("未启用");
}

std::string VehicleLocker::comboStatusText() const {
    std::vector<std::string> items;
    if (fuelEnabled()) items.push_back("无限油量");
    if (damageEnabled()) items.push_back("无损");
    if (antiRollEnabled()) items.push_back("防侧翻");
    if (items.empty()) return "车辆锁定已关闭";
    std::string text;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) text += " + ";
        text += items[i];
    }
    text += "锁定中";
    return text;
}

void VehicleLocker::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = text;
}

std::string VehicleLocker::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

}  // namespace ets2
