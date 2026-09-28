// vehicle.h —— ETS2 1.61 固定 RVA + 动态指针链车辆锁定（不做数值扫描）
#pragma once

#include "memory.h"
#include "telemetry.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ets2 {

struct VehicleAddresses {
    uint64_t rootGlobal = 0;
    uint64_t context = 0;
    uint64_t truck = 0;
    uint64_t fuel = 0;
    uint64_t fuelCorrection = 0;
    std::vector<uint64_t> damageFields;
    // 防侧翻（方案B）相关地址：全部由指针链/模块基址动态推导
    uint64_t truckCenterOfMassY = 0;   // truck + 0x468 —— 车辆重心 Y 偏移（物理每 tick 读取）
    uint64_t truckStabilityCvar = 0;   // g_truck_stability   缓存 float（cvar+0x118）
    uint64_t trailerStabilityCvar = 0; // g_trailer_stability 缓存 float（cvar+0x118）
};

class VehicleLocker {
public:
    ~VehicleLocker() { stop(); }

    bool bind(ProcessMemory* memory, DWORD pid, std::string* error);
    bool probe(VehicleAddresses* addresses, TelemetrySnapshot* telemetry, std::string* error);
    bool setFuelEnabled(bool enabled, std::string* error);
    bool setDamageEnabled(bool enabled, std::string* error);
    bool setAntiRollEnabled(bool enabled, float factor, std::string* error);
    void setAntiRollFactor(float factor);
    void stop();

    bool fuelEnabled() const { return fuelEnabled_.load(); }
    bool damageEnabled() const { return damageEnabled_.load(); }
    bool antiRollEnabled() const { return antiRollEnabled_.load(); }
    float antiRollFactor() const { return antiRollFactor_.load(); }
    std::string status() const;

private:
    bool locateRootGlobal(std::string* error);
    // 只走指针链 + 字段范围检查，不要求遥测（用于防侧翻单独启用 / 还原路径）
    bool resolveChainOnly(VehicleAddresses* addresses, std::string* error) const;
    // requireTelemetry=false 时跳过遥测交叉校验（防侧翻不依赖油量/磨损遥测）
    bool resolveAndValidate(VehicleAddresses* addresses, TelemetrySnapshot* telemetry,
                            std::string* error, bool requireTelemetry = true) const;
    bool writeOnce(const VehicleAddresses& addresses, bool fuel, bool damage, bool antiRoll,
                   float antiRollFactor, std::string* error) const;
    // 把开启前快照的原始物理值写回游戏（精确还原，不猜测默认值）
    bool restoreAntiRollValues(const VehicleAddresses& addresses);
    void ensureThread();
    void run();
    void setStatus(const std::string& text);
    std::string comboStatusText() const;  // 组合当前各锁定项的状态文案

    ProcessMemory* memory_ = nullptr;
    DWORD pid_ = 0;
    uint64_t rootGlobal_ = 0;
    uint64_t moduleBase_ = 0;
    std::atomic<bool> fuelEnabled_{false};
    std::atomic<bool> damageEnabled_{false};
    std::atomic<bool> antiRollEnabled_{false};
    std::atomic<float> antiRollFactor_{3.0f};
    // 防侧翻关闭/停止时的还原请求：由唯一的写入方（worker 线程）消费，避免并发写竞争
    std::atomic<bool> antiRollRestorePending_{false};
    // 开启前快照的原始值（还原必须精确，不能写猜测值）
    std::atomic<bool> comBackupValid_{false};
    std::atomic<float> originalCoMY_{0.0f};
    std::atomic<bool> cvarBackupValid_{false};
    std::atomic<float> originalTruckStability_{0.5f};
    std::atomic<float> originalTrailerStability_{0.5f};
    std::atomic<bool> stopping_{true};
    std::thread thread_;
    mutable std::mutex statusMutex_;
    std::string status_ = "未启用";
};

}  // namespace ets2
