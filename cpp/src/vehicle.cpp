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
        const bool fuelAlreadyLocked = fuelEnabled_.load() && ratio >= 0.999f;
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
            const bool alreadyLocked = damageEnabled_.load() && expectedWear[index] <= 0.002f;
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

bool VehicleLocker::writeOnce(const VehicleAddresses& addresses, bool fuel, bool damage,
                              bool antiRoll, float antiRollFactor,
                              std::string* error) const {
    const float one = 1.0f;
    const float zero = 0.0f;
    if (fuel && (!memory_->write(addresses.fuel, &one, sizeof(one)) ||
                 !memory_->write(addresses.fuelCorrection, &zero, sizeof(zero)))) {
        if (error) *error = "写入油量字段失败";
        return false;
    }
    if (damage) {
        for (uint64_t address : addresses.damageFields) {
            if (!memory_->write(address, &zero, sizeof(zero))) {
                if (error) *error = "写入损伤字段失败";
                return false;
            }
        }
    }
    if (antiRoll && addresses.truckCenterOfMassY) {
        // ---- 方案B：动态重心 / 防侧翻写入 ----
        // 游戏原生公式：com_offset_y = (1.0f - stability) * chassis_com_height
        //   stability 滑条 0~1，对应重心高度约 +0.375f（默认 0.5）。
        // 本功能写入：target = -(factor - 1) * chassis_com_height
        //   factor 1.5 -> -0.375f，2.0 -> -0.75f，3.0 -> -1.5f，5.0 -> -3.75f。
        // 物理每 tick（RVA 0x6482D7 一带）直接读取该偏移计算侧倾恢复力矩，
        // 重心下沉越深，回正力矩越强（不倒翁效果）。
        float factor = antiRollFactor;
        if (!(factor >= 1.0f)) factor = 1.0f;   // 防御 settings.ini 中的非法值
        if (factor > 6.0f) factor = 6.0f;       // 防止极端值造成悬挂异常/抖动

        if (factor > 1.01f) {  // factor==1.0 视为"等效原厂"，不写重心
            float defaultHeight = 0.75f;  // 读不到底盘描述符时的保守回退值
            uint64_t chassisDesc = 0;
            if (readPointer(*memory_, addresses.truck + kTruckChassisDescriptor,
                            &chassisDesc) &&
                chassisDesc) {
                float readHeight = 0.0f;
                if (readFloat(*memory_, chassisDesc + kChassisDefaultCoMHeight, &readHeight) &&
                    readHeight > 0.05f && readHeight < 5.0f) {
                    defaultHeight = readHeight;
                }
            }
            float targetCoMY = -(factor - 1.0f) * defaultHeight;
            if (targetCoMY < -6.0f) targetCoMY = -6.0f;   // 幅度上限
            if (targetCoMY > -0.2f) targetCoMY = -0.2f;   // 幅度下限

            if (!memory_->write(addresses.truckCenterOfMassY, &targetCoMY,
                                sizeof(targetCoMY))) {
                if (error) *error = "写入车辆重心物理偏移失败";
                return false;
            }
        }

        // 同步刷新车头/挂车稳定性 cvar 缓存 float（RVA 0x02D470A0 / 0x02D47320）。
        // 仅在成功快照过原始值（cvarBackupValid_）后才写：保证任何时刻都能精确还原。
        if (cvarBackupValid_.load() && addresses.truckStabilityCvar &&
            addresses.trailerStabilityCvar) {
            const float stability = factor;
            memory_->write(addresses.truckStabilityCvar, &stability, sizeof(stability));
            memory_->write(addresses.trailerStabilityCvar, &stability, sizeof(stability));
        }
    }
    return true;
}

bool VehicleLocker::setFuelEnabled(bool enabled, std::string* error) {
    if (!enabled) {
        fuelEnabled_.store(false);
        setStatus(comboStatusText());
        return true;
    }
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error) ||
        !writeOnce(addresses, true, false, antiRollEnabled_.load(), antiRollFactor_.load(), error)) {
        fuelEnabled_.store(false);
        return false;
    }
    fuelEnabled_.store(true);
    setStatus(fmt("无限油量已锁定（%.1f / %.1f L）", telemetry.fuel,
                  telemetry.fuelCapacity));
    ensureThread();
    return true;
}

bool VehicleLocker::setDamageEnabled(bool enabled, std::string* error) {
    if (!enabled) {
        damageEnabled_.store(false);
        setStatus(comboStatusText());
        return true;
    }
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error) ||
        !writeOnce(addresses, false, true, antiRollEnabled_.load(), antiRollFactor_.load(), error)) {
        damageEnabled_.store(false);
        return false;
    }
    damageEnabled_.store(true);
    setStatus("车辆五项损伤已清零并锁定");
    ensureThread();
    return true;
}

bool VehicleLocker::setAntiRollEnabled(bool enabled, float factor, std::string* error) {
    // 入参收敛到安全区间
    if (!(factor >= 1.0f)) factor = 1.0f;
    if (factor > 6.0f) factor = 6.0f;
    antiRollFactor_.store(factor);

    if (!enabled) {
        antiRollEnabled_.store(false);
        if (memory_ && memory_->isOpen() && rootGlobal_ &&
            (comBackupValid_.load() || cvarBackupValid_.load())) {
            // 交由 worker 线程（唯一写入方）执行还原，避免与进行中的一轮写值竞争
            antiRollRestorePending_.store(true);
            ensureThread();
            setStatus("防侧翻已关闭，正在还原车辆物理参数…");
        } else {
            setStatus(comboStatusText());
        }
        return true;
    }

    // 启用：只走指针链 + 范围校验，不要求油量/磨损遥测一致（防侧翻与它们无关）
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error, /*requireTelemetry=*/false)) {
        antiRollEnabled_.store(false);
        return false;
    }

    // ---- 首次开启必须先快照原始值，之后任何时刻都可精确还原 ----
    float currentCoMY = 0.0f;
    if (!readFloat(*memory_, addresses.truckCenterOfMassY, &currentCoMY)) {
        if (error) *error = "读取车辆重心偏移失败，已拒绝启用";
        antiRollEnabled_.store(false);
        return false;
    }
    if (!comBackupValid_.load()) {
        originalCoMY_.store(currentCoMY);
        comBackupValid_.store(true);
    }
    if (!cvarBackupValid_.load() && addresses.truckStabilityCvar &&
        addresses.trailerStabilityCvar) {
        float ts = 0.0f, trs = 0.0f;
        if (readFloat(*memory_, addresses.truckStabilityCvar, &ts) &&
            readFloat(*memory_, addresses.trailerStabilityCvar, &trs)) {
            originalTruckStability_.store(ts);
            originalTrailerStability_.store(trs);
            cvarBackupValid_.store(true);
        }
        // 读取失败则 cvarBackupValid_ 保持 false：writeOnce 将跳过 cvar 写入（宁缺毋滥）
    }

    if (!writeOnce(addresses, fuelEnabled_.load(), damageEnabled_.load(), true, factor,
                   error)) {
        antiRollEnabled_.store(false);
        return false;
    }
    antiRollEnabled_.store(true);
    antiRollRestorePending_.store(false);
    setStatus(fmt("防侧翻/不倒翁已锁定（强度 %.1fx，原重心 Y=%.2f 已快照）", factor,
                  currentCoMY));
    ensureThread();
    return true;
}

// 把快照的原始物理值写回游戏。不猜测默认值，保证用户原设置不被改动。
bool VehicleLocker::restoreAntiRollValues(const VehicleAddresses& addresses) {
    if (!memory_ || !memory_->isOpen()) return false;
    bool ok = true;
    if (comBackupValid_.load() && addresses.truckCenterOfMassY) {
        const float coMY = originalCoMY_.load();
        ok = memory_->write(addresses.truckCenterOfMassY, &coMY, sizeof(coMY)) && ok;
    }
    if (cvarBackupValid_.load() && addresses.truckStabilityCvar &&
        addresses.trailerStabilityCvar) {
        const float ts = originalTruckStability_.load();
        const float trs = originalTrailerStability_.load();
        ok = memory_->write(addresses.truckStabilityCvar, &ts, sizeof(ts)) && ok;
        ok = memory_->write(addresses.trailerStabilityCvar, &trs, sizeof(trs)) && ok;
    }
    comBackupValid_.store(false);
    cvarBackupValid_.store(false);
    return ok;
}

void VehicleLocker::setAntiRollFactor(float factor) {
    if (!(factor >= 1.0f)) factor = 1.0f;
    if (factor > 6.0f) factor = 6.0f;
    antiRollFactor_.store(factor);
}

void VehicleLocker::ensureThread() {
    if (thread_.joinable()) return;
    stopping_.store(false);
    thread_ = std::thread([this]() { run(); });
}

void VehicleLocker::run() {
    while (!stopping_.load()) {
        const bool fuel = fuelEnabled_.load();
        const bool damage = damageEnabled_.load();
        const bool antiRoll = antiRollEnabled_.load();
        const bool restorePending = antiRollRestorePending_.load();
        if (!fuel && !damage && !antiRoll && !restorePending) {
            ::Sleep(100);
            continue;
        }

        // ---- 还原分支：worker 是唯一写入方，在此消掉还原请求 ----
        if (!antiRoll && restorePending) {
            VehicleAddresses addresses;
            std::string error;
            if (resolveChainOnly(&addresses, &error) &&
                restoreAntiRollValues(addresses)) {
                antiRollRestorePending_.store(false);
                setStatus("防侧翻已关闭：重心与稳定性参数已还原为开启前的原值。");
            } else {
                // 链断（读档/换图瞬间）时稍后重试；游戏侧重算事件也会自愈
                setStatus("防侧翻还原暂缓：" + error + "（稍后自动重试，换车/读档亦会恢复）");
            }
            ::Sleep(300);
            continue;
        }

        VehicleAddresses addresses;
        TelemetrySnapshot telemetry;
        std::string error;
        // 只有油量/无损锁定才需要遥测交叉校验；防侧翻单独开启时跳过
        if (!resolveAndValidate(&addresses, &telemetry, &error, fuel || damage)) {
            setStatus("锁定已暂停：" + error);
            ::Sleep(500);
            continue;
        }
        antiRollRestorePending_.store(false);  // 重新开启则取消未完成的还原
        const float factor = antiRollFactor_.load();
        if (!writeOnce(addresses, fuel, damage, antiRoll, factor, &error)) {
            setStatus("锁定写入失败：" + error);
        } else {
            std::vector<std::string> activeList;
            if (fuel) activeList.push_back("无限油量");
            if (damage) activeList.push_back("车辆无损");
            if (antiRoll) activeList.push_back(fmt("防侧翻/不倒翁(%.1fx)", factor));
            std::string text;
            for (size_t i = 0; i < activeList.size(); ++i) {
                if (i > 0) text += " + ";
                text += activeList[i];
            }
            text += " 锁定中（固定指针）";
            setStatus(text);
        }
        ::Sleep(250);
    }
}

void VehicleLocker::stop() {
    fuelEnabled_.store(false);
    damageEnabled_.store(false);
    antiRollEnabled_.store(false);
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    // worker 可能没来得及消费还原请求：join 之后此处是唯一线程，安全地补一次还原。
    if (memory_ && memory_->isOpen() && rootGlobal_ &&
        (antiRollRestorePending_.load() || comBackupValid_.load() ||
         cvarBackupValid_.load())) {
        VehicleAddresses addresses;
        std::string error;
        if (resolveChainOnly(&addresses, &error)) {
            restoreAntiRollValues(addresses);
        }
    }
    antiRollRestorePending_.store(false);
    memory_ = nullptr;
    pid_ = 0;
    rootGlobal_ = 0;
    moduleBase_ = 0;
    setStatus("未启用");
}

std::string VehicleLocker::comboStatusText() const {
    std::vector<std::string> items;
    if (fuelEnabled_.load()) items.push_back("无限油量");
    if (damageEnabled_.load()) items.push_back("无损");
    if (antiRollEnabled_.load()) items.push_back("防侧翻");
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
