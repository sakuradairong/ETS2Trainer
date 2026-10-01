// vehicletest.cpp —— 防侧翻备份管理 + 车辆维护周期的离线回归测试
//
// 全部只操作自身进程的合成缓冲区（VirtualAlloc / 堆数组），不接触游戏。
// 覆盖审查问题：
//   2 旧地址/复用地址/跨进程代次写回（身份与内容复核、隔离、不盲目重试）
//   4 关闭请求被过期周期覆盖（统一命令状态机 + 序号 + 可控同步点）
//   5 油量/无损入口绕过防侧翻备份（所有写入走同一周期入口）
//   6 部分写入失败无回滚（事务式写入 + 逐项恢复状态 + 回滚失败保留重试）
//   7 车辆后台周期的写入闸门（可注入守卫）
#include "vehicletest.h"

#include "memory.h"
#include "vehicle.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ets2 {

namespace {

constexpr uint64_t kTruckFuelCapacityOff = 0x190;
constexpr uint64_t kTruckChassisDescriptorOff = 0x200;
constexpr uint64_t kTruckWheelWearCountOff = 0xB0;
constexpr uint64_t kTruckCenterOfMassYOff = 0x468;
constexpr uint64_t kTruckFuelRatioOff = 0x1B8;
constexpr uint64_t kTruckEngineWearOff = 0x1AC;

float readFloatAt(ProcessMemory& mem, uint64_t address) {
    float value = 0.0f;
    if (!mem.read(address, &value, sizeof(value))) return 12345.0f;  // 哨兵：读取失败
    return value;
}

bool writeFloatAt(ProcessMemory& mem, uint64_t address, float value) {
    return mem.write(address, &value, sizeof(value));
}

bool sameFloat(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

// 合成车辆：一块可读写的内存页，带身份字段 + 重心 + 油量字段。
struct FakeTruck {
    std::vector<uint8_t> bytes;
    uint64_t object = 0;
    uint64_t comAddress = 0;
    uint64_t identityChassis = 0;

    FakeTruck(float originalCoMY, uint64_t chassisTag, float fuelCapacity) : bytes(0x500, 0) {
        object = (uint64_t)(uintptr_t)bytes.data();
        comAddress = object + kTruckCenterOfMassYOff;
        identityChassis = chassisTag;
        ::memcpy(bytes.data() + kTruckCenterOfMassYOff, &originalCoMY, sizeof(float));
        ::memcpy(bytes.data() + kTruckFuelCapacityOff, &fuelCapacity, sizeof(float));
        const uint64_t wearCount = 0;
        ::memcpy(bytes.data() + kTruckWheelWearCountOff, &wearCount, sizeof(wearCount));
        const uint64_t descriptor = identityChassis;
        ::memcpy(bytes.data() + kTruckChassisDescriptorOff, &descriptor, sizeof(descriptor));
        const float fuelRatio = 0.5f;
        ::memcpy(bytes.data() + kTruckFuelRatioOff, &fuelRatio, sizeof(fuelRatio));
    }

    VehicleIdentity identity() const {
        VehicleIdentity id;
        ::memcpy(&id.fuelCapacityBits, bytes.data() + kTruckFuelCapacityOff, sizeof(uint32_t));
        id.chassisDescriptor = identityChassis;
        id.wheelWearCountBits = 0;
        return id;
    }
};

// 组装一个可用于 runMaintenanceCycle 的地址集合
VehicleAddresses addressesFor(const FakeTruck& truck) {
    VehicleAddresses addresses;
    addresses.truck = truck.object;
    addresses.truckCenterOfMassY = truck.object + kTruckCenterOfMassYOff;
    addresses.fuel = truck.object + kTruckFuelRatioOff;
    addresses.fuelCorrection = truck.object + 0x300;  // 合成修正字段（可写页内）
    addresses.damageFields = {truck.object + kTruckEngineWearOff};
    return addresses;
}

// 合成 4 级指针链（rootGlobal → driver → context → truck），让生产代码的
// resolveAndValidate 能在离线环境走通，从而可以驱动真实 worker 周期。
struct SyntheticChain {
    std::vector<uint8_t> slot = std::vector<uint8_t>(8, 0);            // rootGlobal → root
    std::vector<uint8_t> root = std::vector<uint8_t>(0x3400, 0);      // 含 +0x31B0 指针槽
    std::vector<uint8_t> driver = std::vector<uint8_t>(0x40, 0);      // +0x18 → context
    std::vector<uint8_t> context = std::vector<uint8_t>(0x1400, 0);   // +0x1F8 → truck
    FakeTruck            truck{0.40f, 0x1000, 1000.0f};
    uint64_t             rootGlobal = 0;

    SyntheticChain() {
        const uint64_t rootAddr = (uint64_t)(uintptr_t)root.data();
        const uint64_t driverAddr = (uint64_t)(uintptr_t)driver.data();
        const uint64_t contextAddr = (uint64_t)(uintptr_t)context.data();
        const uint64_t truckAddr = truck.object;
        // rootGlobal 指向的槽里保存「根对象」指针（readPointer(rootGlobal) → root）
        ::memcpy(slot.data(), &rootAddr, sizeof(rootAddr));
        ::memcpy(root.data() + 0x31B0, &driverAddr, sizeof(driverAddr));
        ::memcpy(driver.data() + 0x18, &contextAddr, sizeof(contextAddr));
        ::memcpy(context.data() + 0x1F8, &truckAddr, sizeof(truckAddr));
        const float correction = 0.0f;
        ::memcpy(context.data() + 0x1158, &correction, sizeof(correction));
        rootGlobal = (uint64_t)(uintptr_t)slot.data();
    }

    VehicleAddresses addresses() const {
        VehicleAddresses result = addressesFor(truck);
        result.rootGlobal = rootGlobal;
        result.context = (uint64_t)(uintptr_t)context.data();
        return result;
    }
};

}  // namespace

std::vector<TunerTestItem> runVehicleAntiRollTests() {
    std::vector<TunerTestItem> items;
    auto add = [&items](const std::string& name, bool ok, const std::string& detail) {
        items.push_back({name, ok, detail});
    };

    ProcessMemory mem;
    std::string err;
    if (!mem.open(::GetCurrentProcessId(), "vehicle-test", &err)) {
        add("防侧翻测试环境（打开自身进程）", false, err);
        return items;
    }

    // 两辆车使用不同的原始重心与不同身份指纹
    FakeTruck truckA(0.40f, 0x1000, 1000.0f);
    FakeTruck truckB(0.75f, 0x2000, 700.0f);

    // ---------- A → B：各自恢复自己的原始重心 ----------
    {
        AntiRollBackupManager mgr;
        std::string error;
        bool ok = mgr.captureCenterOfMass(mem, truckA.object, truckA.comAddress, truckA.identity(),
                                         &error);
        if (ok) ok = writeFloatAt(mem, truckA.comAddress, -1.2f);
        if (ok) mgr.noteAppliedCenterOfMass(truckA.object, -1.2f);
        if (ok) ok = mgr.hasRecordFor(truckA.object);
        if (ok) {
            size_t remaining = 1;
            size_t quarantined = 1;
            std::string restoreError;
            mgr.restoreOtherVehicles(mem, truckB.object, &remaining, &quarantined,
                                     &restoreError);
            ok = remaining == 0 && quarantined == 0;
        }
        const float aAfter = readFloatAt(mem, truckA.comAddress);
        if (ok) ok = sameFloat(aAfter, 0.40f);
        if (ok) ok = mgr.captureCenterOfMass(mem, truckB.object, truckB.comAddress,
                                            truckB.identity(), &error);
        if (ok) ok = writeFloatAt(mem, truckB.comAddress, -2.25f);
        if (ok) mgr.noteAppliedCenterOfMass(truckB.object, -2.25f);
        const auto result = mgr.restoreAll(mem, &error);
        const float bAfter = readFloatAt(mem, truckB.comAddress);
        const bool allOk = ok && result == AntiRollBackupManager::RestoreResult::Restored &&
                           sameFloat(bAfter, 0.75f);
        add("防侧翻备份：A→B 各自恢复独立原值（不串车、不假设同重心）", allOk,
            allOk ? "A→0.40、B→0.75 均按各自备份恢复"
                  : ("A=" + std::to_string(aAfter) + " B=" + std::to_string(bAfter) +
                     " result=" + std::to_string((int)result) + " err=" + error));
    }

    // ---------- A → B → A ----------
    {
        AntiRollBackupManager mgr;
        std::string error;
        bool ok = mgr.captureCenterOfMass(mem, truckA.object, truckA.comAddress, truckA.identity(),
                                          &error);
        if (ok) ok = writeFloatAt(mem, truckA.comAddress, -1.2f);
        if (ok) mgr.noteAppliedCenterOfMass(truckA.object, -1.2f);
        if (ok) {
            size_t remaining = 1, quarantined = 1;
            std::string restoreError;
            mgr.restoreOtherVehicles(mem, truckB.object, &remaining, &quarantined, &restoreError);
            ok = remaining == 0 && quarantined == 0;
        }
        if (ok) ok = mgr.captureCenterOfMass(mem, truckB.object, truckB.comAddress,
                                            truckB.identity(), &error);
        if (ok) ok = writeFloatAt(mem, truckB.comAddress, -2.25f);
        if (ok) mgr.noteAppliedCenterOfMass(truckB.object, -2.25f);
        if (ok) {
            size_t remaining = 1, quarantined = 1;
            std::string restoreError;
            mgr.restoreOtherVehicles(mem, truckA.object, &remaining, &quarantined, &restoreError);
            ok = remaining == 0 && quarantined == 0;
        }
        const float bRestored = readFloatAt(mem, truckB.comAddress);
        if (ok) ok = sameFloat(bRestored, 0.75f);
        if (ok) ok = mgr.captureCenterOfMass(mem, truckA.object, truckA.comAddress,
                                            truckA.identity(), &error);
        float aOriginal = 0.0f;
        for (const AntiRollRecord& record : mgr.records()) {
            if (record.truckObject == truckA.object) aOriginal = record.com.original;
        }
        if (ok) ok = sameFloat(aOriginal, 0.40f);
        if (ok) ok = writeFloatAt(mem, truckA.comAddress, -1.2f);
        if (ok) mgr.noteAppliedCenterOfMass(truckA.object, -1.2f);
        const auto result = mgr.restoreAll(mem, &error);
        const float aAfter = readFloatAt(mem, truckA.comAddress);
        const bool allOk = ok && result == AntiRollBackupManager::RestoreResult::Restored &&
                           sameFloat(aAfter, 0.40f);
        add("防侧翻备份：A→B→A 不叠加、不误用他人原值", allOk,
            allOk ? "切回 A 后以 0.40 为基准并正确恢复"
                  : ("A原值=" + std::to_string(aOriginal) + " B恢复=" +
                     std::to_string(bRestored) + " err=" + error));
    }

    // ---------- 审查问题 2：旧对象失效（页面 decommit）→ 不写入；重新映射后哨兵必须保持不变 ----------
    {
        AntiRollBackupManager mgr;
        std::string error;
        const SIZE_T pageSize = 0x1000;
        uint8_t* page = (uint8_t*)::VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_READWRITE);
        if (!page) {
            add("防侧翻备份：失效地址绝不重放（哨兵保持不变）", false, "VirtualAlloc 失败");
        } else {
            const uint64_t truck = (uint64_t)(uintptr_t)page;
            const uint64_t com = truck + kTruckCenterOfMassYOff;
            const float original = 0.5f, modified = -1.5f, sentinel = 123.25f;
            ::memcpy(page + kTruckCenterOfMassYOff, &original, sizeof(original));
            const float capacity = 900.0f;
            ::memcpy(page + kTruckFuelCapacityOff, &capacity, sizeof(capacity));
            const uint64_t wearCount = 0;
            ::memcpy(page + kTruckWheelWearCountOff, &wearCount, sizeof(wearCount));
            const uint64_t descriptor = 0x3000;
            ::memcpy(page + kTruckChassisDescriptorOff, &descriptor, sizeof(descriptor));

            VehicleIdentity identity;
            bool ok = readVehicleIdentity(mem, truck, &identity);
            if (ok) ok = mgr.captureCenterOfMass(mem, truck, com, identity, &error);
            if (ok) ok = writeFloatAt(mem, com, modified);
            if (ok) mgr.noteAppliedCenterOfMass(truck, modified);
            // 对象失效：decommit 该页（读不到 → 只能重试，绝不写入）
            if (ok) ok = ::VirtualFree(page, pageSize, MEM_DECOMMIT) != 0;
            const auto unavailable = mgr.restoreAll(mem, &error);
            // 同一地址重新映射成「新对象」并放入哨兵值
            uint8_t* replacement =
                (uint8_t*)::VirtualAlloc(page, pageSize, MEM_COMMIT, PAGE_READWRITE);
            if (!replacement) {
                add("防侧翻备份：失效地址绝不重放（哨兵保持不变）", false,
                    "重新映射失败");
            } else {
                ::memcpy(replacement + kTruckCenterOfMassYOff, &sentinel, sizeof(sentinel));
                // 注意：身份字段仍是原来那套（模拟「地址被复用但身份字段未变」的最坏情况）——
                // 此时必须靠「字段内容既不是原值也不是我们写入的值」把记录隔离。
                const auto replay = mgr.restoreAll(mem, &error);
                const float after = readFloatAt(mem, com);
                const bool sentinelKept = sameFloat(after, sentinel);
                const bool noFalseSuccess =
                    replay != AntiRollBackupManager::RestoreResult::Restored;
                const bool quarantined = !mgr.quarantined().empty();
                const bool allOk = ok && unavailable == AntiRollBackupManager::RestoreResult::Pending &&
                                   sentinelKept && noFalseSuccess && quarantined;
                add("防侧翻备份：失效地址绝不重放（哨兵保持不变）", allOk,
                    fmt("不可读时=%d 重映射后=%d 哨兵=%.2f（期望 %.2f）隔离=%d",
                        (int)unavailable, (int)replay, (double)after, (double)sentinel,
                        (int)mgr.quarantined().size()));
                ::VirtualFree(replacement, 0, MEM_RELEASE);
            }
        }
    }

    // ---------- 审查问题 2：同址「新车辆」（身份指纹不同）必须隔离，不覆盖新对象 ----------
    {
        AntiRollBackupManager mgr;
        std::string error;
        std::vector<uint8_t> buffer(0x500, 0);
        const uint64_t object = (uint64_t)(uintptr_t)buffer.data();
        const uint64_t com = object + kTruckCenterOfMassYOff;
        const float original = 0.42f, modified = -1.26f, newCarCoM = 0.9f;
        ::memcpy(buffer.data() + kTruckCenterOfMassYOff, &original, sizeof(original));
        const float capacity = 1000.0f;
        ::memcpy(buffer.data() + kTruckFuelCapacityOff, &capacity, sizeof(capacity));
        const uint64_t wearCount = 0;
        ::memcpy(buffer.data() + kTruckWheelWearCountOff, &wearCount, sizeof(wearCount));
        const uint64_t descriptorA = 0x4000;
        ::memcpy(buffer.data() + kTruckChassisDescriptorOff, &descriptorA, sizeof(descriptorA));

        VehicleIdentity identityA;
        bool ok = readVehicleIdentity(mem, object, &identityA);
        if (ok) ok = mgr.captureCenterOfMass(mem, object, com, identityA, &error);
        if (ok) ok = writeFloatAt(mem, com, modified);
        if (ok) mgr.noteAppliedCenterOfMass(object, modified);
        // 同一地址被「新车」复用：身份指纹变化 + 字段变成新车的真实值
        const uint64_t descriptorB = 0x5000;
        ::memcpy(buffer.data() + kTruckChassisDescriptorOff, &descriptorB, sizeof(descriptorB));
        ::memcpy(buffer.data() + kTruckCenterOfMassYOff, &newCarCoM, sizeof(newCarCoM));
        const auto result = mgr.restoreAll(mem, &error);
        const float after = readFloatAt(mem, com);
        const bool okAll = ok && !sameFloat(after, original) && sameFloat(after, newCarCoM) &&
                           result == AntiRollBackupManager::RestoreResult::Unverified &&
                           !mgr.quarantined().empty();
        add("防侧翻备份：同址新车辆（身份不符）已隔离、不覆盖", okAll,
            fmt("新值=%.3f（期望 %.3f）result=%d 隔离=%d", (double)after, (double)newCarCoM,
                (int)result, (int)mgr.quarantined().size()));
    }

    // ---------- 审查问题 2：进程代次变化不得向新进程重放旧地址 ----------
    {
        AntiRollBackupManager mgr;
        std::string error;
        FakeTruck truckC(0.33f, 0x6000, 800.0f);
        bool ok = mgr.beginProcessGeneration(1) == 0;
        if (ok) ok = mgr.captureCenterOfMass(mem, truckC.object, truckC.comAddress,
                                            truckC.identity(), &error);
        if (ok) ok = writeFloatAt(mem, truckC.comAddress, -0.99f);
        if (ok) mgr.noteAppliedCenterOfMass(truckC.object, -0.99f);
        if (ok) ok = mgr.beginProcessGeneration(2) >= 1;  // 重新附加 → 新代次
        // 旧进程地址即使仍可读，也不得写回
        const auto result = mgr.restoreAll(mem, &error);
        const float after = readFloatAt(mem, truckC.comAddress);
        const bool okAll = ok && sameFloat(after, -0.99f) &&
                           result != AntiRollBackupManager::RestoreResult::Restored &&
                           !mgr.quarantined().empty() && mgr.processGeneration() == 2;
        add("防侧翻备份：跨进程代次不重放旧地址", okAll,
            fmt("旧地址值=%.3f（应保持 -0.99）result=%d 隔离=%d 代次=%llu", (double)after,
                (int)result, (int)mgr.quarantined().size(),
                (unsigned long long)mgr.processGeneration()));
    }

    // ---------- 审查问题 6：部分写入失败必须回滚；回滚失败要保留恢复记录 ----------
    {
        // 场景一：读回失败（写入后值被改）→ 整个事务回滚到写入前
        AntiRollBackupManager mgr;
        std::string error;
        FakeTruck truckD(0.55f, 0x7000, 1200.0f);
        VehicleIdentity identity;
        bool ok = readVehicleIdentity(mem, truckD.object, &identity);
        if (ok) ok = mgr.captureCenterOfMass(mem, truckD.object, truckD.comAddress, identity, &error);
        const float before = readFloatAt(mem, truckD.comAddress);
        // 直接调用管理器路径验证「内容不符 → 隔离」，并验证事务回滚在 VehicleLocker 里
        const bool isolationOk =
            ok && writeFloatAt(mem, truckD.comAddress, 9.99f) &&
            mgr.restoreAll(mem, &error) == AntiRollBackupManager::RestoreResult::Unverified &&
            !mgr.quarantined().empty();
        add("防侧翻备份：内容被改写的记录被隔离而非盲写", isolationOk,
            isolationOk ? "字段内容与记录不符 → 隔离，未写回"
                        : fmt("隔离失败（before=%.3f）", (double)before));
    }

    // ---------- 审查问题 4/5：统一命令状态机 + 周期语义（共享实现） ----------
    {
        VehicleLocker locker;
        const VehicleCommandState state = locker.commands();
        const bool initialClean = !state.fuel && !state.damage && !state.antiRoll;
        const uint64_t seq0 = state.sequence;
        // 启用与关闭都必须推进序号（用于让过期周期自我作废、丢弃旧任务）
        locker.setCommandsForTesting(false, false, true, 3.0f);
        const uint64_t seq1 = locker.commands().sequence;
        locker.setCommandsForTesting(false, false, false, 3.0f);
        const uint64_t seq2 = locker.commands().sequence;
        locker.setCommandsForTesting(true, true, false, 3.0f);
        const uint64_t seq3 = locker.commands().sequence;
        const bool ok = initialClean && seq1 > seq0 && seq2 > seq1 && seq3 > seq2 &&
                        !locker.antiRollEnabled() && locker.fuelEnabled() &&
                        locker.damageEnabled();
        add("车辆命令状态机：每次状态变化推进序号", ok,
            fmt("seq %llu → %llu → %llu → %llu", (unsigned long long)seq0,
                (unsigned long long)seq1, (unsigned long long)seq2,
                (unsigned long long)seq3));
    }

    // ---------- 审查问题 4：可控同步点「取快照→用户关闭→周期继续」不得写入且最终恢复 ----------
    {
        VehicleLocker locker;
        ProcessMemory* memPtr = &mem;
        FakeTruck* truck = &truckA;
        // 注入真实内存指针：用一个轻量适配把 ProcessMemory 交给 locker
        // （locker 需要 memory_ 非空；这里通过 bind 无法伪造版本指纹，
        //  因此直接使用测试专用的内存绑定入口）
        const bool bound = locker.bindForTesting(memPtr, (uint64_t)(uintptr_t)truck->bytes.data());
        std::string error;
        bool ok = bound;
        // 启用防侧翻（会备份 A 的真实原值 0.40）
        if (ok) {
            locker.setCommandsForTesting(false, false, true, 3.0f);
        }
        if (ok) {
            const VehicleAddresses addresses = addressesFor(*truck);
            VehicleLocker::CycleResult first =
                locker.runMaintenanceCycle(addresses, false, &error);
            ok = first.action == VehicleLocker::CycleAction::Applied;
        }
        const float modifiedValue = readFloatAt(mem, truck->comAddress);
        // 模拟：worker 取到「防侧翻已启用」快照后，用户关闭功能
        if (ok) {
            locker.setCycleHookAfterSnapshot([&locker]() {
                locker.setCommandsForTesting(false, false, false, 3.0f);
            });
            const VehicleAddresses addresses = addressesFor(*truck);
            const VehicleLocker::CycleResult stale =
                locker.runMaintenanceCycle(addresses, false, &error);
            ok = stale.action == VehicleLocker::CycleAction::SkippedStale;
            locker.setCycleHookAfterSnapshot(nullptr);
        }
        const float afterStale = readFloatAt(mem, truck->comAddress);
        if (ok) ok = sameFloat(afterStale, modifiedValue);  // 过期周期没有再写
        // 新周期按最新状态执行还原
        VehicleLocker::CycleResult restore{};
        if (ok) {
            const VehicleAddresses addresses = addressesFor(*truck);
            restore = locker.runMaintenanceCycle(addresses, false, &error);
            ok = restore.action == VehicleLocker::CycleAction::Restored;
        }
        const float afterRestore = readFloatAt(mem, truck->comAddress);
        if (ok) ok = sameFloat(afterRestore, 0.40f);
        const bool allOk = ok && !locker.antiRollEnabled();
        locker.stop();
        add("防侧翻周期：关闭请求不被过期任务覆盖且最终还原原值", allOk,
            allOk ? "过期周期未写入，新周期按记录还原为 0.40"
                  : fmt("action=%d 过期后=%.3f 还原后=%.3f err=%s", (int)restore.action,
                        (double)afterStale, (double)afterRestore, error.c_str()));
    }

    // ---------- 审查问题 5：油量入口不得绕过防侧翻备份（换车后先开油量） ----------
    {
        VehicleLocker locker;
        FakeTruck* truck = &truckB;
        std::string error;
        bool ok = locker.bindForTesting(&mem, (uint64_t)(uintptr_t)truck->bytes.data());
        if (ok) locker.setCommandsForTesting(false, false, true, 3.0f);
        if (ok) {
            // 换到另一辆车（模拟维护线程还没处理）
            FakeTruck* other = &truckA;
            const VehicleAddresses addresses = addressesFor(*other);
            const VehicleLocker::CycleResult applied =
                locker.runMaintenanceCycle(addresses, false, &error);
            ok = applied.action == VehicleLocker::CycleAction::Applied;
        }
        const float otherComWhileEnabled = readFloatAt(mem, truckA.comAddress);
        // 关闭防侧翻 → 必须恢复 A 的真实原值（0.40），而不是被修改过的值
        if (ok) {
            locker.setCommandsForTesting(false, false, false, 3.0f);
            const VehicleAddresses addresses = addressesFor(truckA);
            const VehicleLocker::CycleResult restored =
                locker.runMaintenanceCycle(addresses, false, &error);
            ok = restored.action == VehicleLocker::CycleAction::Restored;
        }
        const float otherComAfter = readFloatAt(mem, truckA.comAddress);
        locker.stop();
        const bool allOk = ok && sameFloat(otherComAfter, 0.40f);
        add("防侧翻备份：换车后油量/无损入口不绕过独立备份", allOk,
            allOk ? "换车后写入仍先备份，关闭时恢复新车真实原值 0.40"
                  : fmt("A 修改中=%.3f 关闭后=%.3f err=%s", (double)otherComWhileEnabled,
                        (double)otherComAfter, error.c_str()));
    }

    // ---------- 审查问题 7：写入闸门拦截后不再写入 ----------
    {
        VehicleLocker locker;
        FakeTruck* truck = &truckA;
        std::string error;
        bool blocked = false;
        bool ok = locker.bindForTesting(&mem, (uint64_t)(uintptr_t)truck->bytes.data());
        locker.setWriteGuard([&blocked]() {
            return blocked ? std::string("测试：检测到联机") : std::string();
        });
        locker.setGuardIntervalMs(0);  // 每周期都复查
        if (ok) {
            const VehicleAddresses addresses = addressesFor(*truck);
            locker.setCommandsForTesting(false, false, true, 3.0f);
            ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                 VehicleLocker::CycleAction::Applied;
        }
        const float enabledValue = readFloatAt(mem, truck->comAddress);
        if (ok) ok = !sameFloat(enabledValue, 0.40f);
        blocked = true;  // 进入联机
        VehicleLocker::CycleResult cycle{};
        if (ok) {
            const VehicleAddresses addresses = addressesFor(*truck);
            cycle = locker.runMaintenanceCycle(addresses, false, &error);
            ok = cycle.action == VehicleLocker::CycleAction::Blocked;
        }
        const float afterBlock = readFloatAt(mem, truck->comAddress);
        const bool recovered = sameFloat(afterBlock, 0.40f);
        // 闸门仍然拦截时，后续周期不得再写入
        VehicleLocker::CycleResult again{};
        if (ok) {
            const VehicleAddresses addresses = addressesFor(*truck);
            again = locker.runMaintenanceCycle(addresses, false, &error);
            ok = again.action == VehicleLocker::CycleAction::Idle ||
                 again.action == VehicleLocker::CycleAction::Blocked;
        }
        const float afterAgain = readFloatAt(mem, truck->comAddress);
        locker.stop();
        const bool allOk = ok && recovered && sameFloat(afterAgain, 0.40f) &&
                           !locker.antiRollEnabled() && !locker.fuelEnabled() &&
                           !locker.damageEnabled();
        add("车辆周期：写入闸门拦截后停止修改并安全恢复", allOk,
            allOk ? "命中闸门 → 停止写入并还原原值；后续周期不再写入"
                  : fmt("cycle=%d 拦截后=%.3f 再次=%.3f err=%s", (int)cycle.action,
                        (double)afterBlock, (double)afterAgain, error.c_str()));
    }

    // ---------- 审查问题 6：事务式写入 —— 分别注入重心 / 车头 cvar / 挂车 cvar 失败 ----------
    {
        // 注入方式：先正常启用（备份 + 写入成功），再把「某一个字段的地址」变成不可访问
        // （decommit），下一周期该字段既读不到也写不进 → 必须回滚本轮已写入的其它字段。
        // 注意：同进程 WriteProcessMemory 会绕过只读页保护，所以不能用 PAGE_READONLY 注入。
        const SIZE_T pageSize = 0x1000;
        uint8_t* identityPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize,
                                                         MEM_COMMIT | MEM_RESERVE,
                                                         PAGE_READWRITE);
        uint8_t* comPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE);
        uint8_t* cvarTruckPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize,
                                                          MEM_COMMIT | MEM_RESERVE,
                                                          PAGE_READWRITE);
        uint8_t* cvarTrailerPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize,
                                                            MEM_COMMIT | MEM_RESERVE,
                                                            PAGE_READWRITE);
        if (!identityPage || !comPage || !cvarTruckPage || !cvarTrailerPage) {
            add("防侧翻事务：注入失败回滚", false, "VirtualAlloc 失败");
        } else {
            const uint64_t truckObject = (uint64_t)(uintptr_t)identityPage;
            const float capacity = 1000.0f;
            ::memcpy(identityPage + kTruckFuelCapacityOff, &capacity, sizeof(capacity));
            const uint64_t wearCount = 0;
            ::memcpy(identityPage + kTruckWheelWearCountOff, &wearCount, sizeof(wearCount));
            const uint64_t descriptor = 0x9000;
            ::memcpy(identityPage + kTruckChassisDescriptorOff, &descriptor,
                     sizeof(descriptor));
            const float comOriginal = 0.60f, cvarTruckOriginal = 0.50f, cvarTrailerOriginal = 0.35f;
            ::memcpy(comPage + kTruckCenterOfMassYOff, &comOriginal, sizeof(comOriginal));
            ::memcpy(cvarTruckPage, &cvarTruckOriginal, sizeof(cvarTruckOriginal));
            ::memcpy(cvarTrailerPage, &cvarTrailerOriginal, sizeof(cvarTrailerOriginal));

            VehicleAddresses addresses;
            addresses.truck = truckObject;
            addresses.truckCenterOfMassY = (uint64_t)(uintptr_t)comPage + kTruckCenterOfMassYOff;
            addresses.truckStabilityCvar = (uint64_t)(uintptr_t)cvarTruckPage;
            addresses.trailerStabilityCvar = (uint64_t)(uintptr_t)cvarTrailerPage;
            const uint64_t comAddress = addresses.truckCenterOfMassY;

            // (A) 重心字段失效 → 事务失败，且失效项必须保留为「需重试」（不伪报成功）
            bool okA = true;
            std::string errorA;
            {
                VehicleLocker locker;
                std::string error;
                okA = locker.bindForTesting(&mem, truckObject);
                locker.setCommandsForTesting(false, false, true, 3.0f);
                if (okA) {
                    okA = locker.runMaintenanceCycle(addresses, false, &error).action ==
                          VehicleLocker::CycleAction::Applied;
                }
                okA = okA && ::VirtualFree(comPage, pageSize, MEM_DECOMMIT) != 0;
                VehicleLocker::CycleResult res{};
                if (okA) {
                    res = locker.runMaintenanceCycle(addresses, false, &error);
                    okA = res.action == VehicleLocker::CycleAction::Failed;
                    errorA = error;
                }
                // 不得伪报成功：失效项必须仍可重试
                if (okA) okA = locker.antiRollBackupForTesting().hasRetryableRestore();
                locker.stop();
            }
            const bool caseA = okA;

            // (B) 车头 cvar 失效 → 本轮事务不得留下半途修改；关闭后重心必须回到真正原值
            bool okB = true;
            std::string errorB;
            {
                ::VirtualAlloc(comPage, pageSize, MEM_COMMIT, PAGE_READWRITE);
                ::memcpy(comPage + kTruckCenterOfMassYOff, &comOriginal, sizeof(comOriginal));
                VehicleLocker locker;
                std::string error;
                okB = locker.bindForTesting(&mem, truckObject);
                locker.setCommandsForTesting(false, false, true, 3.0f);
                if (okB) {
                    okB = locker.runMaintenanceCycle(addresses, false, &error).action ==
                          VehicleLocker::CycleAction::Applied;
                }
                const float comBeforeCycle = readFloatAt(mem, comAddress);
                okB = okB && ::VirtualFree(cvarTruckPage, pageSize, MEM_DECOMMIT) != 0;
                VehicleLocker::CycleResult res{};
                if (okB) {
                    res = locker.runMaintenanceCycle(addresses, false, &error);
                    okB = res.action == VehicleLocker::CycleAction::Failed;
                    errorB = error;
                }
                // 本事务写入的重心必须回滚到「本轮之前」的值（不留半途修改）
                if (okB) okB = sameFloat(readFloatAt(mem, comAddress), comBeforeCycle);
                // 关闭功能：重心必须回到真正原值；失效的 cvar 项保留（不静默丢弃）
                locker.setCommandsForTesting(false, false, false, 3.0f);
                if (okB) {
                    const VehicleLocker::CycleResult restore =
                        locker.runMaintenanceCycle(addresses, false, &error);
                    // 不得报告「已重新应用」；失效项未恢复时必须如实保留待重试
                    okB = restore.action != VehicleLocker::CycleAction::Applied;
                }
                if (okB) okB = sameFloat(readFloatAt(mem, comAddress), comOriginal);
                if (okB) okB = locker.antiRollBackupForTesting().hasRetryableRestore();
                locker.stop();
            }
            const bool caseB = okB;

            // (C) 挂车 cvar 失效 → 重心与车头 cvar 都要回到真正原值
            bool okC = true;
            std::string errorC;
            {
                ::VirtualAlloc(cvarTruckPage, pageSize, MEM_COMMIT, PAGE_READWRITE);
                ::memcpy(cvarTruckPage, &cvarTruckOriginal, sizeof(cvarTruckOriginal));
                VehicleLocker locker;
                std::string error;
                okC = locker.bindForTesting(&mem, truckObject);
                locker.setCommandsForTesting(false, false, true, 3.0f);
                if (okC) {
                    okC = locker.runMaintenanceCycle(addresses, false, &error).action ==
                          VehicleLocker::CycleAction::Applied;
                }
                const float comBeforeCycle = readFloatAt(mem, comAddress);
                const float cvarTruckBeforeCycle = readFloatAt(mem, addresses.truckStabilityCvar);
                okC = okC && ::VirtualFree(cvarTrailerPage, pageSize, MEM_DECOMMIT) != 0;
                VehicleLocker::CycleResult res{};
                if (okC) {
                    res = locker.runMaintenanceCycle(addresses, false, &error);
                    okC = res.action == VehicleLocker::CycleAction::Failed;
                    errorC = error;
                }
                if (okC) {
                    okC = sameFloat(readFloatAt(mem, comAddress), comBeforeCycle) &&
                          sameFloat(readFloatAt(mem, addresses.truckStabilityCvar),
                                    cvarTruckBeforeCycle);
                }
                locker.setCommandsForTesting(false, false, false, 3.0f);
                if (okC) locker.runMaintenanceCycle(addresses, false, &error);
                if (okC) {
                    okC = sameFloat(readFloatAt(mem, comAddress), comOriginal) &&
                          sameFloat(readFloatAt(mem, addresses.truckStabilityCvar),
                                    cvarTruckOriginal);
                }
                // 失效的挂车 cvar 项必须保留为需重试
                if (okC) okC = locker.antiRollBackupForTesting().hasRetryableRestore();
                locker.stop();
            }
            const bool caseC = okC;

            // (D) 读回校验失败（重叠字段制造确定性回读不一致）→ 不得伪报成功
            bool okD = true;
            std::string errorD;
            {
                ::VirtualAlloc(cvarTrailerPage, pageSize, MEM_COMMIT, PAGE_READWRITE);
                ::memcpy(cvarTrailerPage, &cvarTrailerOriginal, sizeof(cvarTrailerOriginal));
                VehicleLocker locker;
                std::string error;
                VehicleAddresses aliased = addresses;
                aliased.truckStabilityCvar = (uint64_t)(uintptr_t)cvarTruckPage;
                aliased.trailerStabilityCvar = (uint64_t)(uintptr_t)cvarTruckPage + 2;
                okD = locker.bindForTesting(&mem, truckObject);
                locker.setCommandsForTesting(false, false, true, 3.0f);
                if (okD) {
                    okD = locker.runMaintenanceCycle(aliased, false, &error).action ==
                          VehicleLocker::CycleAction::Failed;
                    errorD = error;
                }
                // 失败后既不能伪报成功，也不能静默丢失记录
                if (okD) {
                    okD = locker.antiRollBackupForTesting().hasRetryableRestore() ||
                          !locker.antiRollBackupForTesting().quarantined().empty();
                }
                locker.stop();
            }
            const bool caseD = okD;

            add("防侧翻事务：重心/车头cvar/挂车cvar 注入失败与回读失败均回滚且不虚报",
                caseA && caseB && caseC && caseD,
                fmt("A(重心失效)=%d B(车头cvar失效)=%d C(挂车cvar失效)=%d D(回读失败)=%d | %s | %s | %s",
                    (int)caseA, (int)caseB, (int)caseC, (int)caseD, errorA.c_str(),
                    errorB.c_str(), errorC.c_str()));

            ::VirtualFree(cvarTrailerPage, 0, MEM_RELEASE);
            ::VirtualFree(cvarTruckPage, 0, MEM_RELEASE);
            ::VirtualFree(comPage, 0, MEM_RELEASE);
            ::VirtualFree(identityPage, 0, MEM_RELEASE);
        }
    }

    // ---------- 复审 2：正式 bind 与测试绑定共用生命周期；旧进程记录不得重放 ----------
    {
        // 模拟「旧进程已失效、stop 恢复未完成 → 重新绑定」：
        // 旧车辆/旧 cvar 的地址先失效（decommit），再映射新内容并重新绑定。
        const SIZE_T pageSize = 0x1000;
        uint8_t* comPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE);
        uint8_t* cvarPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE);
        bool ok = comPage && cvarPage;
        std::string error;
        const float original = 0.40f, sentinelCom = 0.77f, sentinelCvar = 0.11f;
        uint64_t sentinelComAddr = 0, sentinelCvarAddr = 0;
        uint64_t generationAfterFirstBind = 0, generationAfterSecondBind = 0;
        // 同一个 locker 实例贯穿两次绑定（模拟 UI 在新附加进程上复用同一个 locker）
        FakeTruck truckHolder(original, 0x1000, 1000.0f);
        FakeTruck newTruck(sentinelCom, 0x2000, 900.0f);
        VehicleLocker locker;
        VehicleAddresses addresses;
        if (ok) {
            ::memcpy(comPage + kTruckCenterOfMassYOff, &original, sizeof(original));
            const float cvarOriginal = 0.50f;
            ::memcpy(cvarPage, &cvarOriginal, sizeof(cvarOriginal));
            addresses.truck = truckHolder.object;
            addresses.truckCenterOfMassY = (uint64_t)(uintptr_t)comPage + kTruckCenterOfMassYOff;
            addresses.truckStabilityCvar = (uint64_t)(uintptr_t)cvarPage;
            addresses.trailerStabilityCvar = (uint64_t)(uintptr_t)cvarPage;

            ok = locker.bindForTesting(&mem, truckHolder.object);
            generationAfterFirstBind = locker.antiRollBackupForTesting().processGeneration();
            locker.setCommandsForTesting(false, false, true, 3.0f);
            if (ok) {
                ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                     VehicleLocker::CycleAction::Applied;
            }
            // 旧进程失效：两个地址都不可读（stop() 的还原因此未完成）
            ok = ok && ::VirtualFree(comPage, pageSize, MEM_DECOMMIT) != 0 &&
                 ::VirtualFree(cvarPage, pageSize, MEM_DECOMMIT) != 0;
            locker.stop();
            sentinelComAddr = addresses.truckCenterOfMassY;
            sentinelCvarAddr = addresses.truckStabilityCvar;
        }
        if (ok) {
            // 重新映射出「新进程」的同一批地址，放入哨兵值
            uint8_t* comAgain =
                (uint8_t*)::VirtualAlloc(comPage, pageSize, MEM_COMMIT, PAGE_READWRITE);
            uint8_t* cvarAgain =
                (uint8_t*)::VirtualAlloc(cvarPage, pageSize, MEM_COMMIT, PAGE_READWRITE);
            ok = comAgain && cvarAgain;
            if (ok) {
                ::memcpy(comAgain + kTruckCenterOfMassYOff, &sentinelCom, sizeof(sentinelCom));
                ::memcpy(cvarAgain, &sentinelCvar, sizeof(sentinelCvar));
                // 重新绑定（与生产 bind() 调用同一实现 beginNewProcessGeneration）
                ok = locker.bindForTesting(&mem, newTruck.object);
                generationAfterSecondBind =
                    locker.antiRollBackupForTesting().processGeneration();
                if (ok) {
                    std::string restoreError;
                    const auto result = locker.antiRollBackupForTesting().restoreAll(
                        mem, &restoreError);
                    const float comNow = readFloatAt(mem, sentinelComAddr);
                    const float cvarNow = readFloatAt(mem, sentinelCvarAddr);
                    const bool notReplayed = sameFloat(comNow, sentinelCom) &&
                                             sameFloat(cvarNow, sentinelCvar);
                    const bool noFalseSuccess =
                        result != AntiRollBackupManager::RestoreResult::Restored;
                    const bool advanced = generationAfterSecondBind > generationAfterFirstBind;
                    const bool quarantined = locker.antiRollBackupForTesting().quarantineTotal() > 0;
                    ok = notReplayed && noFalseSuccess && advanced && quarantined;
                    add("防侧翻绑定：同一 locker 重新绑定隔离旧代次（旧车辆与旧 cvar 都不重放）", ok,
                        fmt("代次 %llu→%llu 重心 %.2f（期望 %.2f）cvar %.2f（期望 %.2f）结果=%d 隔离=%llu",
                            (unsigned long long)generationAfterFirstBind,
                            (unsigned long long)generationAfterSecondBind, (double)comNow,
                            (double)sentinelCom, (double)cvarNow, (double)sentinelCvar,
                            (int)result,
                            (unsigned long long)locker.antiRollBackupForTesting()
                                .quarantineTotal()));
                }
                if (comAgain) ::VirtualFree(comAgain, 0, MEM_RELEASE);
                if (cvarAgain) ::VirtualFree(cvarAgain, 0, MEM_RELEASE);
            }
        }
        if (!ok) {
            add("防侧翻绑定：同一 locker 重新绑定隔离旧代次（旧车辆与旧 cvar 都不重放）", false,
                "测试环境构造失败");
        }
    }

    // ---------- 复审 3：3→1→关闭 / 3→1→重新启用 / 连续改强度（有 cvar）都回到最初原值 ----------
    {
        SyntheticChain chain;
        const float comOriginal = 0.40f, cvarOriginal = 0.50f;
        float cvarTruck = cvarOriginal, cvarTrailer = 0.35f;
        const uint64_t cvarTruckAddr = (uint64_t)(uintptr_t)&cvarTruck;
        const uint64_t cvarTrailerAddr = (uint64_t)(uintptr_t)&cvarTrailer;
        VehicleAddresses addresses = chain.addresses();
        addresses.truckStabilityCvar = cvarTruckAddr;
        addresses.trailerStabilityCvar = cvarTrailerAddr;

        VehicleLocker locker;
        std::string error;
        bool ok = locker.bindForTesting(&mem, chain.rootGlobal);
        // 3.0 → 写入重心与 cvar
        locker.setCommandsForTesting(false, false, true, 3.0f);
        if (ok) {
            ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                 VehicleLocker::CycleAction::Applied;
        }
        const float afterThree = readFloatAt(mem, chain.truck.comAddress);
        // 1.0 → 等效原厂：不写数值，立即把本车已写入项还原
        locker.setCommandsForTesting(false, false, true, 1.0f);
        VehicleLocker::CycleResult reduced{};
        if (ok) {
            reduced = locker.runMaintenanceCycle(addresses, false, &error);
            ok = reduced.action == VehicleLocker::CycleAction::Applied;
        }
        const float afterOne = readFloatAt(mem, chain.truck.comAddress);
        if (ok) ok = sameFloat(afterOne, comOriginal);
        // 1.0 期间重新启用 3.0 → 仍以最初原值为基准
        locker.setCommandsForTesting(false, false, true, 3.0f);
        if (ok) {
            ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                 VehicleLocker::CycleAction::Applied;
        }
        float recordedOriginal = 0.0f;
        for (const AntiRollRecord& record : locker.antiRollBackupForTesting().records()) {
            if (record.truckObject == chain.truck.object) recordedOriginal = record.com.original;
        }
        if (ok) ok = sameFloat(recordedOriginal, comOriginal);
        // 连续改强度 2.0 → 1.0 → 3.0（每次都不应污染原值）
        for (float factor : {2.0f, 1.0f, 3.0f}) {
            if (!ok) break;
            locker.setCommandsForTesting(false, false, true, factor);
            const VehicleLocker::CycleResult step =
                locker.runMaintenanceCycle(addresses, false, &error);
            ok = step.action == VehicleLocker::CycleAction::Applied ||
                 step.action == VehicleLocker::CycleAction::Restored;
        }
        for (const AntiRollRecord& record : locker.antiRollBackupForTesting().records()) {
            if (record.truckObject == chain.truck.object && !sameFloat(record.com.original, comOriginal)) {
                ok = false;
            }
        }
        // 关闭 → 重心与 cvar 都必须回到最初原值
        locker.setCommandsForTesting(false, false, false, 3.0f);
        VehicleLocker::CycleResult closed{};
        if (ok) {
            closed = locker.runMaintenanceCycle(addresses, false, &error);
        }
        const float comFinal = readFloatAt(mem, chain.truck.comAddress);
        const float cvarTruckFinal = readFloatAt(mem, cvarTruckAddr);
        const float cvarTrailerFinal = readFloatAt(mem, cvarTrailerAddr);
        locker.stop();
        const bool okFinal = ok && sameFloat(comFinal, comOriginal) &&
                             sameFloat(cvarTruckFinal, cvarOriginal) &&
                             sameFloat(cvarTrailerFinal, 0.35f) &&
                             !locker.antiRollBackupForTesting().hasPendingRestore();
        add("防侧翻强度：3→1→关闭 / 3→1→重启用 / 连续改强度都回到最初原值", okFinal,
            fmt("3x 后=%.2f 1x 后=%.2f 关闭后=%.2f cvar=%.2f/%.2f（期望 0.40/0.50/0.35）动作=%d",
                (double)afterThree, (double)afterOne, (double)comFinal, (double)cvarTruckFinal,
                (double)cvarTrailerFinal, (int)closed.action));
    }

    // ---------- 复审 7：隔离队列已满时仍必须如实返回 Unverified ----------
    {
        AntiRollBackupManager manager;
        manager.beginProcessGeneration(1);
        FakeTruck truck(0.40f, 0x1000, 1000.0f);
        const float unrelated = 0.9f;
        AntiRollBackupManager::RestoreResult single{};
        for (int i = 0; i < 9; ++i) {
            const float original = 0.40f;
            ::memcpy(truck.bytes.data() + kTruckCenterOfMassYOff, &original, sizeof(original));
            if (!manager.captureCenterOfMass(mem, truck.object, truck.comAddress, &err)) break;
            // 外部改写内容（既非原值也非本程序写入值）→ 每轮都会隔离 1 项
            ::memcpy(truck.bytes.data() + kTruckCenterOfMassYOff, &unrelated, sizeof(unrelated));
            single = manager.restoreAll(mem, &err);
        }
        const bool ninthOk = single == AntiRollBackupManager::RestoreResult::Unverified &&
                             manager.quarantineTotal() == 9 && manager.quarantined().size() == 8;

        // 队列满时一次隔离多条：仍必须是 Unverified，且计数准确
        FakeTruck truckA(0.40f, 0x1000, 1000.0f), truckB(0.75f, 0x2000, 700.0f);
        manager.captureCenterOfMass(mem, truckA.object, truckA.comAddress, &err);
        manager.captureCenterOfMass(mem, truckB.object, truckB.comAddress, &err);
        const float unrelatedA = 0.91f, unrelatedB = 0.92f;
        ::memcpy(truckA.bytes.data() + kTruckCenterOfMassYOff, &unrelatedA, sizeof(unrelatedA));
        ::memcpy(truckB.bytes.data() + kTruckCenterOfMassYOff, &unrelatedB, sizeof(unrelatedB));
        const uint64_t totalBefore = manager.quarantineTotal();
        const auto many = manager.restoreAll(mem, &err);
        const bool manyOk = many == AntiRollBackupManager::RestoreResult::Unverified &&
                            manager.quarantineTotal() == totalBefore + 2 &&
                            manager.quarantined().size() == 8;
        // 两条候选都必须保持「外部改写后的值」——绝不能把别人的值改回我们的原值
        const bool notRestored = sameFloat(readFloatAt(mem, truckA.comAddress), unrelatedA) &&
                                 sameFloat(readFloatAt(mem, truckB.comAddress), unrelatedB);
        add("防侧翻隔离：队列满（8 条）后仍如实返回 Unverified", ninthOk && manyOk && notRestored,
            fmt("第 9 次=%d 计数=%llu 队列=%d；批量=%d 计数=%llu 队列=%d", (int)single,
                (unsigned long long)manager.quarantineTotal(), (int)manager.quarantined().size(),
                (int)many, (unsigned long long)manager.quarantineTotal(),
                (int)manager.quarantined().size()));
    }

    // ---------- 复审 4：真正注入「写成功→后续失败→回滚失败→地址重新可读」 ----------
    {
        const SIZE_T pageSize = 0x1000;
        uint8_t* identityPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize,
                                                         MEM_COMMIT | MEM_RESERVE,
                                                         PAGE_READWRITE);
        uint8_t* comPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE);
        uint8_t* cvarPage = (uint8_t*)::VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE);
        bool ok = identityPage && comPage && cvarPage;
        const float comOriginal = 0.60f, cvarOriginal = 0.50f;
        uint64_t comAddress = 0, cvarAddress = 0;
        float appliedValue = 0.0f;
        if (ok) {
            const uint64_t truckObject = (uint64_t)(uintptr_t)identityPage;
            const float capacity = 1000.0f, ratio = 0.5f;
            ::memcpy(identityPage + kTruckFuelCapacityOff, &capacity, sizeof(capacity));
            ::memcpy(identityPage + kTruckFuelRatioOff, &ratio, sizeof(ratio));
            const uint64_t wearCount = 0;
            ::memcpy(identityPage + kTruckWheelWearCountOff, &wearCount, sizeof(wearCount));
            const uint64_t descriptor = 0x9000;
            ::memcpy(identityPage + kTruckChassisDescriptorOff, &descriptor, sizeof(descriptor));
            ::memcpy(comPage + kTruckCenterOfMassYOff, &comOriginal, sizeof(comOriginal));
            ::memcpy(cvarPage, &cvarOriginal, sizeof(cvarOriginal));
            comAddress = (uint64_t)(uintptr_t)comPage + kTruckCenterOfMassYOff;
            cvarAddress = (uint64_t)(uintptr_t)cvarPage;

            VehicleAddresses addresses;
            addresses.truck = truckObject;
            addresses.truckCenterOfMassY = comAddress;
            addresses.truckStabilityCvar = cvarAddress;
            // 稳定性两个字段重叠 → 写完后「整体一致性复验」必然发现车头字段被破坏，
            // 从而触发回滚；再由回滚同步点把两页都置为不可访问 → 回滚失败。
            addresses.trailerStabilityCvar = cvarAddress + 2;

            VehicleLocker locker;
            ok = locker.bindForTesting(&mem, truckObject);
            locker.setRollbackHookForTesting([&]() {
                ::VirtualFree(comPage, pageSize, MEM_DECOMMIT);
                ::VirtualFree(cvarPage, pageSize, MEM_DECOMMIT);
            });
            locker.setCommandsForTesting(false, false, true, 3.0f);
            VehicleLocker::CycleResult failed{};
            if (ok) {
                failed = locker.runMaintenanceCycle(addresses, false, &err);
                ok = failed.action == VehicleLocker::CycleAction::Failed;
            }
            appliedValue = -(3.0f - 1.0f) * 0.75f;  // 合成底盘缺省重心高度 0.75
            // 记账检查：重心项必须是 NeedsRestore 且 applied = 我们刚写入的值
            bool bookkeepingOk = false;
            for (const AntiRollRecord& record : locker.antiRollBackupForTesting().records()) {
                if (record.truckObject != truckObject) continue;
                bookkeepingOk = record.com.state == AntiRollItemState::NeedsRestore &&
                                sameFloat(record.com.applied, appliedValue);
            }
            if (ok) ok = bookkeepingOk;
            if (ok) ok = locker.antiRollBackupForTesting().hasRetryableRestore();
            // 地址重新可读（内存里仍是本程序写入的值）→ 重试必须恢复到真实原值
            uint8_t* comAgain =
                (uint8_t*)::VirtualAlloc(comPage, pageSize, MEM_COMMIT, PAGE_READWRITE);
            uint8_t* cvarAgain =
                (uint8_t*)::VirtualAlloc(cvarPage, pageSize, MEM_COMMIT, PAGE_READWRITE);
            ok = ok && comAgain && cvarAgain;
            if (ok) {
                ::memcpy(comAgain + kTruckCenterOfMassYOff, &appliedValue,
                         sizeof(appliedValue));
                ::memcpy(cvarAgain, &cvarOriginal, sizeof(cvarOriginal));
                std::string retryError;
                const auto retried = locker.antiRollBackupForTesting().restoreAll(mem, &retryError);
                const float comNow = readFloatAt(mem, comAddress);
                const bool restoredTrue = sameFloat(comNow, comOriginal);
                const bool noQuarantine = locker.antiRollBackupForTesting().quarantineTotal() == 0;
                const bool honest = retried != AntiRollBackupManager::RestoreResult::Unverified;
                locker.stop();
                add("防侧翻事务：回滚失败保留真实写入值，地址恢复后可还原真实原值", restoredTrue &&
                                                                                     noQuarantine &&
                                                                                     honest,
                    fmt("失败动作=%d 记账=%d 重试结果=%d 恢复值=%.2f（期望 %.2f）隔离=%llu",
                        (int)failed.action, (int)bookkeepingOk, (int)retried, (double)comNow,
                        (double)comOriginal,
                        (unsigned long long)locker.antiRollBackupForTesting().quarantineTotal()));
                ::VirtualFree(comAgain, 0, MEM_RELEASE);
                ::VirtualFree(cvarAgain, 0, MEM_RELEASE);
            } else {
                locker.stop();
                add("防侧翻事务：回滚失败保留真实写入值，地址恢复后可还原真实原值", false,
                    fmt("构造失败：动作=%d 记账=%d 待重试=%d err=%s", (int)failed.action,
                        (int)bookkeepingOk,
                        (int)locker.antiRollBackupForTesting().hasRetryableRestore(),
                        err.c_str()));
            }
        } else {
            add("防侧翻事务：回滚失败保留真实写入值，地址恢复后可还原真实原值", false,
                "VirtualAlloc 失败");
        }
        if (identityPage) ::VirtualFree(identityPage, 0, MEM_RELEASE);
    }

    // ---------- 复审 6：worker 链断→准备恢复→UI 重新启用（最新命令优先、凭据不丢） ----------
    {
        VehicleLocker locker;
        FakeTruck truck(0.40f, 0x1000, 1000.0f);
        std::string error;
        // 用一个非法根指针模拟「指针链断开」
        bool ok = locker.bindForTesting(&mem, 0x1000);
        const VehicleAddresses addresses = addressesFor(truck);
        // 先用可用地址建立记录并写入（不经过 resolve，直接驱动周期主逻辑）
        locker.setCommandsForTesting(false, false, true, 3.0f);
        if (ok) {
            ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                 VehicleLocker::CycleAction::Applied;
        }
        const float modified = readFloatAt(mem, truck.comAddress);
        if (ok) ok = !sameFloat(modified, 0.40f);
        // 关闭功能；worker 迭代时链断 → 走链断恢复；同步点让 UI 重新启用
        locker.setCommandsForTesting(false, false, false, 3.0f);
        locker.setCycleHookAfterSnapshot([&locker]() {
            locker.setCommandsForTesting(false, false, true, 3.0f);  // UI 重新启用
        });
        VehicleLocker::CycleResult stale{};
        if (ok) {
            stale = locker.runWorkerIteration();
            ok = stale.action == VehicleLocker::CycleAction::SkippedStale;
        }
        locker.setCycleHookAfterSnapshot(nullptr);
        const float afterStale = readFloatAt(mem, truck.comAddress);
        if (ok) ok = sameFloat(afterStale, modified);  // 没有按过期状态还原（凭据保留）
        float originalKept = 0.0f;
        for (const AntiRollRecord& record : locker.antiRollBackupForTesting().records()) {
            if (record.truckObject == truck.object) originalKept = record.com.original;
        }
        if (ok) ok = sameFloat(originalKept, 0.40f);
        // 真正关闭后（无注入）：链断也应完成还原
        locker.setCommandsForTesting(false, false, false, 3.0f);
        VehicleLocker::CycleResult restored{};
        if (ok) {
            restored = locker.runWorkerIteration();
            ok = restored.action == VehicleLocker::CycleAction::Restored ||
                 restored.action == VehicleLocker::CycleAction::Idle;
        }
        const float finalValue = readFloatAt(mem, truck.comAddress);
        locker.stop();
        const bool okFinal = ok && sameFloat(finalValue, 0.40f);
        add("防侧翻链断：worker 恢复与 UI 重启用交错时以最新命令为准", okFinal,
            fmt("过期动作=%d 过期后=%.2f 原值=%.2f 最终动作=%d 最终=%.2f", (int)stale.action,
                (double)afterStale, (double)originalKept, (int)restored.action,
                (double)finalValue));
    }

    // ---------- 复审 5：worker 持续运行时反复替换守卫与启停，不得竞争或漏过禁写 ----------
    {
        SyntheticChain chain;
        VehicleLocker locker;
        std::string error;
        std::atomic<bool> deny{false};
        bool ok = locker.bindForTesting(&mem, chain.rootGlobal);
        locker.setWriteGuard([&deny]() {
            return deny.load() ? std::string("测试：联机中") : std::string();
        });
        locker.setGuardIntervalMs(0);  // 每轮都复查
        // 生产入口启动 worker（解析合成指针链 → 备份 → 写入 → ensureThread）
        if (ok) ok = locker.setAntiRollEnabled(true, 3.0f, &error);
        if (ok) ok = locker.antiRollEnabled();
        ::Sleep(500);
        const float modified = readFloatAt(mem, chain.truck.comAddress);
        if (ok) ok = !sameFloat(modified, 0.40f);   // worker 在写入
        // 进入联机：worker 必须停止修改并还原
        deny.store(true);
        ::Sleep(800);
        const float afterDeny = readFloatAt(mem, chain.truck.comAddress);
        const bool restoredWhileDenied = sameFloat(afterDeny, 0.40f);
        // 联机期间写入哨兵值：不得被 worker 覆盖成修改值
        const float sentinel = 0.123f;
        writeFloatAt(mem, chain.truck.comAddress, sentinel);
        ::Sleep(800);
        const float sentinelAfter = readFloatAt(mem, chain.truck.comAddress);
        const bool notOverwritten = sameFloat(sentinelAfter, sentinel);
        // 反复替换守卫 + 启停（模拟运行中替换守卫的数据竞争窗口）
        for (int round = 0; round < 12; ++round) {
            locker.setWriteGuard([&deny]() {
                return deny.load() ? std::string("测试：联机中") : std::string();
            });
            locker.setCommandsForTesting(false, false, (round % 2) == 0, 3.0f);
            ::Sleep(60);
        }
        // 回到单机并启用：worker 恢复写入
        deny.store(false);
        locker.setCommandsForTesting(false, false, true, 3.0f);
        ::Sleep(900);
        const float resumed = readFloatAt(mem, chain.truck.comAddress);
        const bool resumedWriting = !sameFloat(resumed, sentinel);
        locker.stop();
        add("车辆守卫：worker 运行中替换守卫/启停不竞争且不漏禁写", ok && restoredWhileDenied &&
                                                                     notOverwritten &&
                                                                     resumedWriting,
            fmt("写入=%d 拦截后=%.3f（期望 0.400）哨兵保持=%d 恢复写入=%d（%.3f）err=%s",
                (int)!sameFloat(modified, 0.40f), (double)afterDeny, (int)notOverwritten,
                (int)resumedWriting, (double)resumed, error.c_str()));
    }

    // ---------- 审查问题 4：快速开关与「恢复期间重新启用」 ----------
    {
        VehicleLocker locker;
        FakeTruck* truck = &truckA;
        std::string error;
        bool ok = locker.bindForTesting(&mem, (uint64_t)(uintptr_t)truck->bytes.data());
        if (ok) ok = sameFloat(readFloatAt(mem, truck->comAddress), 0.40f);
        const VehicleAddresses addresses = addressesFor(*truck);
        // 快速开关 5 轮：每轮开→写、关→还原，最终必须回到真正原值
        for (int round = 0; ok && round < 5; ++round) {
            locker.setCommandsForTesting(false, false, true, 3.0f);
            ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                 VehicleLocker::CycleAction::Applied;
            locker.setCommandsForTesting(false, false, false, 3.0f);
            const VehicleLocker::CycleResult restore =
                locker.runMaintenanceCycle(addresses, false, &error);
            ok = restore.action == VehicleLocker::CycleAction::Restored;
        }
        if (ok) ok = sameFloat(readFloatAt(mem, truck->comAddress), 0.40f);
        // 恢复期间重新启用：关闭后不先跑还原周期，直接再开 → 必须仍以最初原值为基准
        locker.setCommandsForTesting(false, false, true, 3.0f);
        if (ok) {
            ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                 VehicleLocker::CycleAction::Applied;
        }
        locker.setCommandsForTesting(false, false, false, 3.0f);   // 请求还原（尚未执行）
        locker.setCommandsForTesting(false, false, true, 3.0f);    // 还原完成前重新启用
        if (ok) {
            ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                 VehicleLocker::CycleAction::Applied;
        }
        float original = 0.0f;
        for (const AntiRollRecord& record : locker.antiRollBackupForTesting().records()) {
            if (record.truckObject == truck->object) original = record.com.original;
        }
        // 关键：重新启用后原值仍是 0.40，而不是被修改过的值
        if (ok) ok = sameFloat(original, 0.40f);
        locker.setCommandsForTesting(false, false, false, 3.0f);
        if (ok) {
            const VehicleLocker::CycleResult restore =
                locker.runMaintenanceCycle(addresses, false, &error);
            ok = restore.action == VehicleLocker::CycleAction::Restored;
        }
        if (ok) ok = sameFloat(readFloatAt(mem, truck->comAddress), 0.40f);
        locker.stop();
        add("防侧翻周期：快速开关与恢复期间重新启用仍以最初原值为基准", ok,
            ok ? "5 轮开关 + 还原前重启用，最终全部回到 0.40"
               : fmt("原值=%.3f 当前=%.3f err=%s", (double)original,
                     (double)readFloatAt(mem, truck->comAddress), error.c_str()));
    }

    // ---------- 无待恢复项时不得报告「恢复成功」 ----------
    {
        AntiRollBackupManager mgr;
        std::string error;
        const auto result = mgr.restoreAll(mem, &error);
        add("防侧翻备份：无备份时如实返回 NothingToDo",
            result == AntiRollBackupManager::RestoreResult::NothingToDo,
            "没有执行任何恢复时不冒充成功");
    }

    // ---------- 内存不可用（进程关闭）：不得报告成功且记录保留 ----------
    {
        AntiRollBackupManager mgr;
        std::string error;
        FakeTruck truckE(0.44f, 0x8000, 900.0f);
        VehicleIdentity identity;
        bool ok = readVehicleIdentity(mem, truckE.object, &identity);
        if (ok) ok = mgr.captureCenterOfMass(mem, truckE.object, truckE.comAddress, identity, &error);
        if (ok) ok = writeFloatAt(mem, truckE.comAddress, -1.32f);
        if (ok) mgr.noteAppliedCenterOfMass(truckE.object, -1.32f);
        ProcessMemory dead;  // 从未打开的句柄
        const auto result = mgr.restoreAll(dead, &error);
        float keptOriginal = 0.0f;
        for (const AntiRollRecord& record : mgr.records()) {
            if (record.truckObject == truckE.object) keptOriginal = record.com.original;
        }
        const bool allOk = ok && result == AntiRollBackupManager::RestoreResult::Pending &&
                           sameFloat(keptOriginal, 0.44f) && mgr.hasRetryableRestore();
        add("防侧翻备份：内存不可用时失败且记录保留可重试", allOk,
            allOk ? "返回 Pending，原值 0.44 仍保留"
                  : ("result=" + std::to_string((int)result) + " err=" + error));
        mgr.restoreAll(mem, &error);  // 收尾
    }

    // ---------- 第三轮：恢复请求不绕过写入凭据/内容校验 ----------
    {
        FakeTruck truck(0.4f, 0x1000, 1000.0f);
        float truckCvar = 0.5f, trailerCvar = 0.35f;
        VehicleAddresses addresses = addressesFor(truck);
        addresses.truckStabilityCvar = (uint64_t)(uintptr_t)&truckCvar;
        addresses.trailerStabilityCvar = (uint64_t)(uintptr_t)&trailerCvar;
        VehicleLocker locker;
        std::string error;
        bool ok = locker.bindForTesting(&mem, truck.object);
        locker.setGuardIntervalMs(0);
        locker.setCommandsForTesting(false, false, true, 3.0f);
        if (ok) ok = locker.runMaintenanceCycle(addresses, false, &error).action ==
                     VehicleLocker::CycleAction::Applied;
        const bool changed = writeFloatAt(mem, truck.comAddress, 0.9f) &&
                             writeFloatAt(mem, addresses.truckStabilityCvar, 0.11f) &&
                             writeFloatAt(mem, addresses.trailerStabilityCvar, 0.22f);
        locker.setWriteGuard([] { return std::string("测试：联机中"); });
        const auto blocked = locker.runMaintenanceCycle(addresses, false, &error);
        ok = ok && changed && blocked.action == VehicleLocker::CycleAction::Blocked &&
             !locker.antiRollEnabled() && sameFloat(readFloatAt(mem, truck.comAddress), 0.9f) &&
             sameFloat(truckCvar, 0.11f) && sameFloat(trailerCvar, 0.22f) &&
             locker.antiRollBackupForTesting().quarantineTotal() > 0 &&
             !locker.antiRollBackupForTesting().hasPendingRestore();
        locker.stop();
        add("防侧翻恢复：联机守卫不覆盖外部改动的重心/两个 cvar", ok,
            fmt("CoM=%.2f cvar=%.2f/%.2f 动作=%d（外部值必须保持）", readFloatAt(mem, truck.comAddress),
                truckCvar, trailerCvar, (int)blocked.action));
    }
    {
        AntiRollBackupManager mgr;
        FakeTruck truck(0.4f, 0x1000, 1000.0f);
        float truckCvar = 0.5f, trailerCvar = 0.35f;
        std::string error;
        bool ok = mgr.captureCenterOfMass(mem, truck.object, truck.comAddress, &error) &&
                  mgr.captureCvars(mem, (uint64_t)(uintptr_t)&truckCvar,
                                   (uint64_t)(uintptr_t)&trailerCvar, &error);
        ok = writeFloatAt(mem, truck.comAddress, 0.9f) && ok;
        truckCvar = 0.11f;
        trailerCvar = 0.22f;
        mgr.markNeedsRestore(truck.object);  // 从未写入，不可凭请求升级为已写入。
        mgr.markCenterOfMassPendingRestore(truck.object, -1.5f);
        mgr.markStabilityPendingRestore(3.0f, 3.0f);
        mgr.noteCenterOfMassValueInMemory(truck.object, 0.9f); // 仅观察外部值不生成证据。
        mgr.noteStabilityValuesInMemory(0.11f, 0.22f);
        const auto records = mgr.records();
        ok = ok && records.size() == 1 && records[0].com.state == AntiRollItemState::Captured &&
             !records[0].com.hasApplied && !records[0].com.hasAcknowledged;
        const auto result = mgr.restoreAll(mem, &error);
        ok = ok && result == AntiRollBackupManager::RestoreResult::Unverified &&
             sameFloat(readFloatAt(mem, truck.comAddress), 0.9f) && sameFloat(truckCvar, 0.11f) &&
             sameFloat(trailerCvar, 0.22f);
        add("防侧翻恢复：仅 Captured 的字段不因请求或观察而生成写入证据", ok, error);
    }
    {
        AntiRollBackupManager mgr;
        FakeTruck truck(0.4f, 0x1000, 1000.0f);
        float truckCvar = 0.5f, trailerCvar = 0.35f;
        const uint64_t truckField = (uint64_t)(uintptr_t)&truckCvar;
        const uint64_t trailerField = (uint64_t)(uintptr_t)&trailerCvar;
        std::string error;
        bool ok = mgr.captureCenterOfMass(mem, truck.object, truck.comAddress, &error) &&
                  mgr.captureCvars(mem, truckField, trailerField, &error);
        const bool wrote = writeFloatAt(mem, truckField, 3.0f);
        if (wrote) mgr.noteFieldWriteSucceeded(truckField, 3.0f);
        mgr.noteFieldValueVerified(truckField, readFloatAt(mem, truckField));
        const auto records = mgr.records();
        ok = ok && wrote && records.size() == 1 && records[0].truckCvar.hasApplied &&
             !records[0].trailerCvar.hasApplied && !records[0].trailerCvar.hasAcknowledged &&
             records[0].trailerCvar.state == AntiRollItemState::Captured;
        trailerCvar = 0.22f;
        mgr.markStabilityPendingRestore(3.0f, 3.0f);
        const auto result = mgr.restoreAll(mem, &error);
        ok = ok && result == AntiRollBackupManager::RestoreResult::Unverified &&
             sameFloat(truckCvar, 0.5f) && sameFloat(trailerCvar, 0.22f);
        add("防侧翻记账：成功写入车头 cvar 不给未写的挂车生成证据", ok, error);
        const uint64_t quarantines = mgr.quarantineTotal();
        const bool recaptured = mgr.captureCvars(mem, truckField, trailerField, &error);
        mgr.markStabilityPendingRestore(3.0f, 3.0f);
        mgr.noteAppliedStability(3.0f, 3.0f);
        mgr.noteFieldWriteSucceeded(trailerField, 3.0f);
        mgr.noteFieldValueVerified(trailerField, 3.0f);
        const auto again = mgr.restoreAll(mem, &error);
        add("防侧翻隔离：恢复请求/其他字段记账不能重新激活隔离项",
            !recaptured && !mgr.cvarCaptured() &&
            again == AntiRollBackupManager::RestoreResult::NothingToDo &&
            mgr.quarantineTotal() == quarantines && !mgr.hasPendingRestore() &&
            sameFloat(trailerCvar, 0.22f), "隔离后不重试、不覆盖外部值");
    }
    {
        auto* page = (uint8_t*)::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        AntiRollBackupManager mgr;
        float trailer = 0.35f;
        std::string error;
        bool ok = page != nullptr;
        if (page) {
            const uint64_t truckField = (uint64_t)(uintptr_t)page;
            const uint64_t trailerField = (uint64_t)(uintptr_t)&trailer;
            const float original = 0.5f;
            ::memcpy(page, &original, sizeof(original));
            ok = mgr.captureCvars(mem, truckField, trailerField, &error) &&
                 writeFloatAt(mem, truckField, 3.0f) && writeFloatAt(mem, trailerField, 3.0f);
            mgr.noteAppliedStability(3.0f, 3.0f);
            mgr.markNeedsRestore(0);
            DWORD protection = 0;
            const bool protectedPage = ::VirtualProtect(page, 0x1000, PAGE_NOACCESS, &protection) != 0;
            const auto pending = mgr.restoreAll(mem, &error);
            DWORD ignored = 0;
            const bool readable = ::VirtualProtect(page, 0x1000, PAGE_READWRITE, &ignored) != 0;
            const bool captured = mgr.captureCvars(mem, truckField, trailerField, &error);
            const auto restored = mgr.restoreAll(mem, &error);
            ok = ok && protectedPage && readable && captured &&
                 pending == AntiRollBackupManager::RestoreResult::Pending &&
                 restored == AntiRollBackupManager::RestoreResult::Restored &&
                 sameFloat(readFloatAt(mem, truckField), 0.5f) && sameFloat(trailer, 0.35f);
            ::VirtualFree(page, 0, MEM_RELEASE);
        }
        add("防侧翻 cvar：重新捕获已恢复字段不覆盖另一字段的待恢复原值", ok, error);
    }
    // 同样的代次/型号指纹：只有内容仍等于成功发出的值才允许延迟验证/恢复。
    for (int fieldIndex : {0, 1, 2}) for (bool verified : {false, true}) for (bool foreign : {false, true}) {
        auto* page = (uint8_t*)::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                            PAGE_READWRITE);
        AntiRollBackupManager mgr;
        std::string error;
        bool ok = page != nullptr;
        AntiRollBackupManager::RestoreResult result = AntiRollBackupManager::RestoreResult::NothingToDo;
        if (page) {
            FakeTruck model(0.4f, 0x1000, 1000.0f);
            ::memcpy(page, model.bytes.data(), model.bytes.size());
            const uint64_t object = (uint64_t)(uintptr_t)page;
            const uint64_t offset = fieldIndex == 0 ? kTruckCenterOfMassYOff : 0x480 + (fieldIndex - 1) * 4;
            const uint64_t field = object + offset;
            const float original = fieldIndex == 0 ? 0.4f : fieldIndex == 1 ? 0.5f : 0.35f;
            const float written = fieldIndex == 0 ? -1.5f : 3.0f;
            const float truckOriginal = 0.5f, trailerOriginal = 0.35f;
            ::memcpy(page + 0x480, &truckOriginal, sizeof(float));
            ::memcpy(page + 0x484, &trailerOriginal, sizeof(float));
            mgr.beginProcessGeneration(1);
            ok = mgr.captureCenterOfMass(mem, object, object + kTruckCenterOfMassYOff, &error) &&
                 mgr.captureCvars(mem, object + 0x480, object + 0x484, &error);
            const bool wrote = writeFloatAt(mem, field, written);
            if (wrote) mgr.noteFieldWriteSucceeded(field, written);
            if (wrote && verified) mgr.noteFieldValueVerified(field, readFloatAt(mem, field));
            mgr.markFieldPendingRestore(field);
            const auto records = mgr.records();
            if (records.size() == 1) {
                const AntiRollItem& item = fieldIndex == 0 ? records[0].com :
                    fieldIndex == 1 ? records[0].truckCvar : records[0].trailerCvar;
                ok = ok && wrote && item.hasApplied == verified && item.hasAcknowledged != verified;
            } else ok = false;
            const bool decommitted = ::VirtualFree(page, 0x1000, MEM_DECOMMIT) != 0;
            const auto pending = mgr.restoreAll(mem, &error);
            auto* replacement = (uint8_t*)::VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE);
            ok = ok && decommitted && pending == AntiRollBackupManager::RestoreResult::Pending &&
                 replacement != nullptr;
            if (replacement) {
                ::memcpy(replacement, model.bytes.data(), model.bytes.size());
                ::memcpy(replacement + 0x480, &truckOriginal, sizeof(float));
                ::memcpy(replacement + 0x484, &trailerOriginal, sizeof(float));
                const float current = foreign ? 123.25f : written;
                ::memcpy(replacement + offset, &current, sizeof(current));
                result = mgr.restoreAll(mem, &error);
                ok = ok && !mgr.hasPendingRestore() &&
                     (foreign ? result == AntiRollBackupManager::RestoreResult::Unverified &&
                                sameFloat(readFloatAt(mem, field), 123.25f) && mgr.quarantineTotal() > 0
                              : result == AntiRollBackupManager::RestoreResult::Restored &&
                                sameFloat(readFloatAt(mem, field), original) && mgr.quarantineTotal() == 0);
            }
            ::VirtualFree(page, 0, MEM_RELEASE);
        }
        add(foreign ? "防侧翻恢复：NeedsRestore 同址同型号重映射也不覆盖哨兵"
                    : "防侧翻恢复：成功发出但暂未读回的写入经内容复核后可恢复",
            ok, fmt("field=%d verified=%d result=%d 隔离=%llu %s", fieldIndex, (int)verified, (int)result,
                    (unsigned long long)mgr.quarantineTotal(), error.c_str()));
    }
    {
        VehicleLocker locker;
        uint64_t root = 0;
        bool ok = locker.bindForTesting(&mem, (uint64_t)(uintptr_t)&root);
        locker.setCommandsForTesting(false, false, true, 3.0f);
        const uint64_t sequence = locker.commands().sequence;
        for (int i = 0; ok && i < 32; ++i) {
            const auto cycle = locker.runWorkerIteration();
            ok = cycle.action == VehicleLocker::CycleAction::Blocked &&
                 cycle.note.find("暂停") != std::string::npos;
        }
        ok = ok && locker.commands().sequence == sequence;
        locker.stop();
        add("防侧翻链断：命令未变化时暂停而非 SkippedStale", ok,
            "连续链断返回可等待状态，真实重新启用的过期命令测试仍保留");
    }
    {
        SyntheticChain chain;
        std::atomic<uint64_t> iterations{0};
        VehicleLocker locker;
        std::string error;
        bool ok = locker.bindForTesting(&mem, chain.rootGlobal);
        locker.setCycleHookAfterSnapshot([&iterations] { iterations.fetch_add(1); });
        if (ok) ok = locker.setAntiRollEnabled(true, 3.0f, &error);
        const uint64_t rootAddress = (uint64_t)(uintptr_t)chain.root.data();
        const uint64_t nullRoot = 0;
        const uint64_t before = iterations.load();
        ok = mem.write(chain.rootGlobal, &nullRoot, sizeof(nullRoot)) && ok;
        ::Sleep(550);
        const uint64_t pausedIterations = iterations.load() - before;
        ok = ok && pausedIterations > 0 && pausedIterations <= 4 &&
             locker.status().find("暂停") != std::string::npos;
        // 恢复指针后必须继续工作，不能只是把 worker 永久停掉来规避忙循环。
        ok = writeFloatAt(mem, chain.truck.comAddress, 0.85f) && ok;
        ok = mem.write(chain.rootGlobal, &rootAddress, sizeof(rootAddress)) && ok;
        const auto waitForValue = [&](float expected) {
            const uint64_t deadline = ::GetTickCount64() + 1500;
            do {
                if (sameFloat(readFloatAt(mem, chain.truck.comAddress), expected)) return true;
                ::Sleep(20);
            } while (::GetTickCount64() < deadline);
            return false;
        };
        ok = waitForValue(-1.5f) && ok;
        const bool closed = locker.setAntiRollEnabled(false, 3.0f, &error);
        ok = closed && waitForValue(0.4f) && ok;
        locker.stop();
        add("防侧翻 worker：链断有界等待、恢复指针后继续工作并还原真实原值", ok,
            fmt("链断 550ms 内迭代=%llu（上限 4），最终重心=%.2f %s",
                (unsigned long long)pausedIterations,
                readFloatAt(mem, chain.truck.comAddress), error.c_str()));
    }

    for (float original : {7.2436189e22f, -100.0f, 6.0f}) {
        FakeTruck truck(original, 0x1000, 1000.0f);
        VehicleLocker locker;
        std::string error;
        const bool bound = locker.bindForTesting(&mem, truck.object);
        locker.setCommandsForTesting(false, false, true, 3.0f);
        const auto result = locker.runMaintenanceCycle(addressesFor(truck), false, &error);
        add("防侧翻异常重心：有限但越界也拒绝捕获与写入",
            bound && result.action == VehicleLocker::CycleAction::Failed &&
            readFloatAt(mem, truck.comAddress) == original &&
            !locker.antiRollBackupForTesting().hasPendingRestore(), error);
        locker.stop();
    }
    for (bool closing : {false, true}) {
        FakeTruck truck(0.4f, 0x1000, 1000.0f);
        VehicleLocker locker;
        std::string error;
        bool ok = locker.bindForTesting(&mem, truck.object);
        const auto addresses = addressesFor(truck);
        locker.setCommandsForTesting(false, false, true, 3.0f);
        ok = locker.runMaintenanceCycle(addresses, false, &error).action == VehicleLocker::CycleAction::Applied && ok;
        locker.setCommandsForTesting(false, false, !closing, 1.0f);
        locker.setCycleHookAfterSnapshot([&locker] { locker.setCommandsForTesting(false, false, true, 3.0f); });
        const auto stale = locker.runMaintenanceCycle(addresses, false, &error);
        const auto records = locker.antiRollBackupForTesting().records();
        ok = ok && stale.action == VehicleLocker::CycleAction::SkippedStale &&
             sameFloat(readFloatAt(mem, truck.comAddress), -1.5f) && records.size() == 1 &&
             sameFloat(records[0].com.original, 0.4f);
        locker.setCycleHookAfterSnapshot(nullptr);
        locker.setCommandsForTesting(false, false, false, 3.0f);
        const auto restored = locker.runMaintenanceCycle(addresses, false, &error);
        ok = ok && restored.action == VehicleLocker::CycleAction::Restored &&
             sameFloat(readFloatAt(mem, truck.comAddress), 0.4f);
        locker.stop();
        add(closing ? "正常车辆链：关闭快照后重新启用，不执行过期恢复"
                    : "正常车辆链：强度 1 快照后改回 3，不执行过期恢复", ok, error);
    }
    mem.close();
    return items;
}

}  // namespace ets2
