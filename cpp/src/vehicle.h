// vehicle.h — verified vehicle fuel and damage locking.
#pragma once
#include "memory.h"
#include "telemetry.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace ets2 {
struct VehicleAddresses {
    uint64_t rootGlobal = 0, context = 0, truck = 0, fuel = 0, fuelCorrection = 0;
    std::vector<uint64_t> damageFields;
};
struct VehicleCommandState {
    bool fuel = false, damage = false;
    uint64_t sequence = 0;
};
class VehicleLocker {
public:
    using WriteGuard = std::function<std::string()>;
    ~VehicleLocker() { stop(); }
    bool bind(ProcessMemory*, DWORD, std::string*);
    bool probe(VehicleAddresses*, TelemetrySnapshot*, std::string*);
    bool setFuelEnabled(bool, std::string*);
    bool setDamageEnabled(bool, std::string*);
    void setWriteGuard(WriteGuard);
    void setGuardIntervalMs(uint64_t);
    void stop();
    bool fuelEnabled() const;
    bool damageEnabled() const;
    std::string status() const;
    enum class CycleAction { Idle, Applied, SkippedStale, Blocked, Failed };
    struct CycleResult { CycleAction action = CycleAction::Idle; std::string note; };
    CycleResult runMaintenanceCycle(const VehicleAddresses&, bool, std::string*);
    CycleResult runWorkerIteration();
    void setCycleHookAfterSnapshot(std::function<void()>);
    void setRollbackHookForTesting(std::function<void()>);
    void setBeforeFieldWriteHookForTesting(std::function<void(size_t)> hook) { beforeFieldWrite_ = std::move(hook); }
    bool bindForTesting(ProcessMemory*, uint64_t);
    void setCommandsForTesting(bool fuel, bool damage);
    VehicleCommandState commands() const;
private:
    bool locateRootGlobal(std::string*);
    bool resolveChainOnly(VehicleAddresses*, std::string*) const;
    bool resolveAndValidate(VehicleAddresses*, TelemetrySnapshot*, std::string*, bool requireTelemetry = true) const;
    void applyCommand(bool fuel, bool damage);
    uint64_t commandSequence() const;
    bool applyTransactional(const VehicleAddresses&, bool fuel, bool damage, uint64_t sequence, std::string*);
    CycleResult runCycleLocked(const VehicleAddresses&, bool, std::string*);
    std::string guardReason();
    void ensureThread();
    void run();
    void setStatus(const std::string&);
    std::string comboStatusText() const;
    ProcessMemory* memory_ = nullptr;
    DWORD pid_ = 0;
    uint64_t rootGlobal_ = 0;
    mutable std::mutex commandMutex_;
    VehicleCommandState command_;
    std::mutex writeMutex_;
    mutable std::mutex guardMutex_;
    WriteGuard guard_;
    uint64_t guardCheckedAt_ = 0, guardCheckedSequence_ = 0, guardIntervalMs_ = 5000;
    std::function<void()> cycleHookAfterSnapshot_, rollbackHookForTesting_;
    std::function<void(size_t)> beforeFieldWrite_;
    std::atomic<bool> stopping_{true};
    std::thread thread_;
    mutable std::mutex statusMutex_;
    std::string status_ = "未启用";
};
} // namespace ets2
