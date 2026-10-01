// vehicle.h —— ETS2 1.61 固定 RVA + 动态指针链车辆锁定（不做数值扫描）
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

// 车辆身份指纹：用于在恢复前确认「这个地址现在还是原来那辆车」。
// 仅凭地址相同或 PID 相同都不足以证明对象身份 —— 必须比对从车辆对象里读到的
// 稳定字段（底盘描述符指针 / 油箱容量 / 磨损数组元素个数）。
struct VehicleIdentity {
    uint64_t chassisDescriptor = 0;   // truck + 0x200（底盘描述符指针）
    uint32_t fuelCapacityBits = 0;    // truck + 0x190（油箱容量 float 的位模式）
    uint32_t wheelWearCountBits = 0;  // truck + 0xB0（磨损数组元素个数）

    bool operator==(const VehicleIdentity& other) const {
        return chassisDescriptor == other.chassisDescriptor &&
               fuelCapacityBits == other.fuelCapacityBits &&
               wheelWearCountBits == other.wheelWearCountBits;
    }
    bool operator!=(const VehicleIdentity& other) const { return !(*this == other); }
};

// 从车辆对象读取身份指纹；任一字段读不到时返回 false（身份未知）。
bool readVehicleIdentity(const ProcessMemory& memory, uint64_t truckObject,
                         VehicleIdentity* identity);

// 防侧翻单个字段的恢复状态机。
//   None        : 无记录
//   Captured    : 已记录原值，尚未写入
//   Applied     : 已成功写入（applied 记录最后写入值）
//   NeedsRestore: 请求还原；仍必须核对写入凭据与当前内容，不代表可以盲写
//   Restored    : 已还原并读回验证，可以安全清除
//   Quarantined : 无法确认身份/内容，已隔离，不再盲目重试，也不冒充恢复成功
enum class AntiRollItemState { None, Captured, Applied, NeedsRestore, Restored, Quarantined };

struct AntiRollItem {
    uint64_t      address = 0;
    float         original = 0.0f;   // 开启前的原值
    float         applied = 0.0f;    // 本程序最后成功写入的值
    bool          hasApplied = false; // applied 已经读回确认（0 本身不是凭据）
    float         acknowledged = 0.0f; // 最近一次 WriteProcessMemory 成功发出的值
    bool          hasAcknowledged = false; // 尚未读回，恢复时必须再次比对内容
    AntiRollItemState state = AntiRollItemState::None;
    bool          active() const {
        return state == AntiRollItemState::Captured || state == AntiRollItemState::Applied ||
               state == AntiRollItemState::NeedsRestore;
    }
};

// 一辆车的防侧翻记录：绑定进程代次 + 车辆身份 + 字段地址 + 最后成功写入值。
struct AntiRollRecord {
    uint64_t        generation = 0;      // 进程代次：跨进程（重新附加）后旧记录不得重放
    uint64_t        truckObject = 0;     // 车辆对象基址
    VehicleIdentity identity;            // 车辆身份指纹（恢复前必须复核）
    bool            identityKnown = false;
    AntiRollItem    com;                 // truck + 0x468（重心 Y 偏移）
    AntiRollItem    truckCvar;           // g_truck_stability 缓存 float（进程级地址）
    AntiRollItem    trailerCvar;         // g_trailer_stability 缓存 float
};

// 被隔离的记录（保留诊断信息，不再参与写入/重试，也不计入恢复成功）。
struct AntiRollQuarantine {
    uint64_t    generation = 0;
    uint64_t    truckObject = 0;
    uint64_t    address = 0;
    float       original = 0.0f;
    float       applied = 0.0f;
    std::string reason;
};

// 防侧翻备份管理：每辆车一条独立记录（与车辆对象/身份绑定），cvar 逐项独立记录。
// 线程安全：内部持锁且临界区极短（只做几次内存读写），不阻塞渲染线程。
class AntiRollBackupManager {
public:
    // 同时保留的车辆记录上限（超出时拒绝新建并保留旧记录）
    static constexpr size_t kMaxVehicleRecords = 8;
    // 隔离记录上限（只保留最近的诊断信息；成败判断用单调计数，绝不用队列长度）
    static constexpr size_t kMaxQuarantine = 8;

    // 恢复结果：
    //   Pending   : 仍有项需要重试（记录保留）
    //   Unverified: 有项无法确认身份/内容，已隔离（不得冒充成功）
    //   Restored  : 全部待恢复项都已写回并读回验证
    //   NothingToDo: 本来就没有待恢复项
    enum class RestoreResult { NothingToDo, Restored, Pending, Unverified };

    // 步入新的进程代次：旧代次的记录一律隔离（不向新进程重放旧地址）。
    // 返回被隔离的记录数。
    size_t beginProcessGeneration(uint64_t generation);
    uint64_t processGeneration() const;

    // 为指定车辆记录原始重心（连同身份指纹）。该车辆已有记录时沿用（绝不覆盖）；
    // 记录数达上限时拒绝并保留现有记录。
    bool captureCenterOfMass(ProcessMemory& memory, uint64_t truckObject, uint64_t comAddress,
                             const VehicleIdentity& identity, std::string* error);
    // 兼容重载：自行从车辆对象读取身份指纹；读不到时拒绝记录（不写入、不猜身份）。
    bool captureCenterOfMass(ProcessMemory& memory, uint64_t truckObject, uint64_t comAddress,
                             std::string* error);
    // 记录稳定性 cvar 原值（进程级，只记录一次；失败保持未捕获，写入端会跳过 cvar）。
    bool captureCvars(ProcessMemory& memory, uint64_t truckCvar, uint64_t trailerCvar,
                      std::string* error);
    // 记录「本程序刚成功写入并读回验证过的值」。**只更新本轮确实写入的字段**：
    // 未写入的字段绝不用占位值覆盖已有凭据（历史缺陷：强度 1.0 只写 cvar 时把已写过的
    // 重心 applied 抹成 0，关闭时无法还原真实原值）。
    void noteAppliedCenterOfMass(uint64_t truckObject, float comAppliedY);
    void noteAppliedStability(float truckStabilityApplied, float trailerStabilityApplied);
    // 兼容旧签名的重载（供既有外部探针/旧调用使用）：**0.0f 视为「该字段本轮未写入」**。
    // 重心目标恒在 [-6,-0.2]、稳定性目标恒在 [1,6]，因此 0 不可能是合法写入值。
    // 生产路径一律使用上面两个精确接口，避免「用占位 0 覆盖已有凭据」。
    void noteApplied(uint64_t truckObject, float comAppliedY, float truckStabilityApplied,
                     float trailerStabilityApplied);
    // 生产事务按地址逐项记账；不得把一个 cvar 的成功写入记到另一个未写字段上。
    // 只有内存 API 返回成功才记录 acknowledged；读回一致后再确认 applied。
    void noteFieldWriteSucceeded(uint64_t address, float value);
    void noteFieldValueVerified(uint64_t address, float value);
    void markFieldPendingRestore(uint64_t address);
    // 兼容恢复请求：不凭传入的目标值创造写入证据，也不重新激活隔离记录。
    void markCenterOfMassPendingRestore(uint64_t truckObject, float appliedValue);
    void markStabilityPendingRestore(float truckApplied, float trailerApplied);
    // 记录「回滚成功后的实际内存值」：等于原值则视为已回到原厂（Captured），
    // 否则仍是本程序写入的值（Applied，applied=该值）。
    void noteCenterOfMassValueInMemory(uint64_t truckObject, float valueInMemory);
    void noteStabilityValuesInMemory(float truckValueInMemory, float trailerValueInMemory);
    // 仅把已有写入凭据的项标记为「请求还原」；Captured 不升级为已写入。
    void markNeedsRestore(uint64_t truckObject);

    // 换车/读档：尝试把「其它车辆」的记录写回它们各自的地址。
    // 只有身份复核通过且字段内容仍是本程序写入值时才写入；否则隔离（不盲目重试）。
    // remaining 返回仍未恢复（需要重试）的项数；quarantined 返回本次隔离数。
    void restoreOtherVehicles(ProcessMemory& memory, uint64_t currentTruck, size_t* remaining,
                              size_t* quarantined, std::string* error);
    // 还原「指定车辆」的项（强度 1.0 = 等效原厂时使用）：逐项验证后写回。
    RestoreResult restoreTruck(ProcessMemory& memory, uint64_t truckObject, std::string* error);
    // 关闭/退出：把所有待恢复项逐项写回验证（结果语义见枚举）。
    RestoreResult restoreAll(ProcessMemory& memory, std::string* error);

    bool   hasPendingRestore() const;
    bool   hasRetryableRestore() const;   // 是否有「还需重试」的项（隔离项不算）
    bool   cvarCaptured() const;
    bool   hasRecordFor(uint64_t truckObject) const;
    bool   identityMatchesFor(uint64_t truckObject, const VehicleIdentity& identity) const;
    size_t pendingVehicleCount() const;
    std::vector<AntiRollRecord> records() const;
    std::vector<AntiRollQuarantine> quarantined() const;
    // 累计隔离次数（单调递增，不受诊断队列长度上限影响 —— 用它判断本轮是否发生隔离）
    uint64_t quarantineTotal() const;

private:
    static void setVerifiedValue(AntiRollItem& item, float value);
    static void verifyRecordedValue(AntiRollItem& item, float value);
    static void requestRestore(AntiRollItem& item);
    void forItemsAtAddressLocked(uint64_t address,
                                const std::function<void(AntiRollItem&)>& update);
    static bool writeFloatVerified(ProcessMemory& memory, uint64_t address, float value);
    void quarantineLocked(const AntiRollRecord& record, const AntiRollItem& item,
                          const std::string& reason);
    void quarantineItemLocked(AntiRollRecord* record, AntiRollItem* item,
                              const std::string& reason);
    // 单项恢复。Restored=已还原 / Retry=暂时不可读需重试 / Quarantined=无法确认已隔离
    enum class ItemOutcome { Restored, Retry, Quarantined };
    ItemOutcome restoreItemLocked(ProcessMemory& memory, AntiRollRecord& record,
                                  AntiRollItem* item, bool needIdentity, std::string* detail);

    mutable std::mutex              mutex_;
    uint64_t                        generation_ = 0;
    std::vector<AntiRollRecord>     vehicles_;   // 每辆车一条
    AntiRollRecord                  cvar_;       // 进程级 cvar（truckObject 保持 0）
    std::vector<AntiRollQuarantine> quarantine_;
    uint64_t                        quarantineTotal_ = 0;  // 单调计数（诊断队列有上限）
};

// 油量 / 无损 / 防侧翻的统一命令状态（单一状态机，避免多个原子 bool 组合出竞态）。
// worker 线程与界面线程都通过它读取「当前该做什么」。
struct VehicleCommandState {
    bool     fuel = false;
    bool     damage = false;
    bool     antiRoll = false;
    float    antiRollFactor = 3.0f;
    uint64_t sequence = 0;  // 每次状态变化自增；用于丢弃「已过期」的周期
};

class VehicleLocker {
public:
    // 写入闸门：返回空串表示允许写入，否则是拒绝原因（联机保护等）。
    // 在真正执行写入前调用；测试可注入。
    using WriteGuard = std::function<std::string()>;

    ~VehicleLocker() { stop(); }

    bool bind(ProcessMemory* memory, DWORD pid, std::string* error);
    bool probe(VehicleAddresses* addresses, TelemetrySnapshot* telemetry, std::string* error);
    bool setFuelEnabled(bool enabled, std::string* error);
    bool setDamageEnabled(bool enabled, std::string* error);
    bool setAntiRollEnabled(bool enabled, float factor, std::string* error);
    void setAntiRollFactor(float factor);
    void setWriteGuard(WriteGuard guard);
    void setGuardIntervalMs(uint64_t milliseconds);  // 测试用
    void stop();

    bool fuelEnabled() const;
    bool damageEnabled() const;
    bool antiRollEnabled() const;
    float antiRollFactor() const;
    std::string status() const;

    // 单个维护周期的结果（worker 线程与测试共用同一实现）。
    enum class CycleAction {
        Idle,          // 无启用项，也无待恢复项
        Applied,       // 已按当前命令写入
        SkippedStale,  // 准备期间命令已变化：本轮不写入（丢弃过期任务）
        Restored,      // 已完成待恢复项的还原
        RestorePending,// 仍有待恢复项需要重试
        Unverified,    // 有项无法确认身份/内容，已隔离
        Blocked,       // 被写入闸门拦下（并已尝试安全恢复）
        Failed         // 备份/写入/回滚失败
    };
    struct CycleResult {
        CycleAction action = CycleAction::Idle;
        std::string note;
    };
    // 单周期主逻辑（真实共享实现）：命令快照 → 闸门 → 换车/备份 → 事务式写入/还原。
    // requireTelemetry 为真时要求油量/磨损遥测交叉校验通过才写入。
    CycleResult runMaintenanceCycle(const VehicleAddresses& addresses, bool requireTelemetry,
                                    std::string* error);
    // 单个 worker 迭代（真实共享实现）：解析指针链失败时会走「链断安全恢复」路径
    // （同样取 writeMutex_ 并复核最新命令）。worker 循环调用它，离线测试直接驱动它。
    CycleResult runWorkerIteration();
    // 供测试注入：在周期中「取命令快照之后、写入之前」调用，用来模拟用户关闭功能。
    void setCycleHookAfterSnapshot(std::function<void()> hook);
    // 供测试注入：在事务即将回滚之前调用（用于确定性注入「回滚失败」）。
    void setRollbackHookForTesting(std::function<void()> hook);

    // ---- 测试专用入口（仅用于离线测试，不参与生产路径）----
    // 绑定内存与假根指针（跳过版本指纹），走与生产 bind() 相同的生命周期
    // （包含进程代次推进），因此测试入口不含任何独有安全逻辑。
    bool bindForTesting(ProcessMemory* memory, uint64_t rootGlobal);
    // 直接设置命令状态（走同一个 applyCommand 状态机，不启动后台线程）。
    void setCommandsForTesting(bool fuel, bool damage, bool antiRoll, float factor);

    // 测试/诊断用快照。
    AntiRollBackupManager& antiRollBackupForTesting() { return antiRollBackup_; }
    VehicleCommandState commands() const;

private:
    bool locateRootGlobal(std::string* error);
    // 只走指针链 + 字段范围检查，不要求遥测（用于防侧翻单独启用 / 还原路径）
    bool resolveChainOnly(VehicleAddresses* addresses, std::string* error) const;
    // requireTelemetry=false 时跳过遥测交叉校验（防侧翻不依赖油量/磨损遥测）
    bool resolveAndValidate(VehicleAddresses* addresses, TelemetrySnapshot* telemetry,
                            std::string* error, bool requireTelemetry = true) const;
    // 更新命令状态（sequence 自增）
    void applyCommand(bool fuel, bool damage, bool antiRoll, float factor);
    uint64_t commandSequence() const;
    // 事务式写入：逐项写入 + 回读验证；任一项失败回滚本轮已写入项（回读验证），
    // 回滚失败则把对应项标记为 NeedsRestore 以便安全重试。
    bool applyTransactional(const VehicleAddresses& addresses, bool fuel, bool damage,
                            bool antiRoll, float factor, std::string* error);
    // 写入 + 回读验证（回滚/还原用）
    bool writeFloatVerifiedSafe(uint64_t address, float value);
    // 周期主逻辑（调用方必须持有 writeMutex_）
    CycleResult runCycleLocked(const VehicleAddresses& addresses, bool requireTelemetry,
                               std::string* error);
    // 写入前的闸门检查（按间隔复查；命令变化后立即复查一次）。
    // 守卫以「加锁复制 + 锁外调用」的方式使用，避免运行中替换守卫的 std::function 竞争。
    std::string guardReason();
    // 进入新的进程代次（生产 bind() 与测试绑定共用，二者不含各自独有的安全逻辑）
    void beginNewProcessGeneration();
    // 任何写入失败后：若存在待恢复项就确保后台线程在跑（否则无人重试）
    void scheduleRestoreIfNeeded();
    void ensureThread();
    void run();
    void setStatus(const std::string& text);
    std::string comboStatusText() const;  // 组合当前各锁定项的状态文案

    ProcessMemory* memory_ = nullptr;
    DWORD pid_ = 0;
    uint64_t rootGlobal_ = 0;
    uint64_t moduleBase_ = 0;
    // 统一命令状态（取代原先的 fuel/damage/antiRoll/restorePending 多个原子 bool）
    mutable std::mutex commandMutex_;
    VehicleCommandState command_;
    // 唯一写入方互斥：界面线程与 worker 线程的所有游戏写入/备份都串行化在这里
    std::mutex writeMutex_;
    // 防侧翻原始值备份：与车辆对象/身份绑定，逐项「写回 + 读回验证」后才清除
    AntiRollBackupManager antiRollBackup_;
    uint64_t nextGeneration_ = 0;
    // 写入闸门（联机保护），可注入：加锁复制后调用，避免运行中替换造成竞争
    mutable std::mutex guardMutex_;
    WriteGuard guard_;
    uint64_t guardCheckedAt_ = 0;
    uint64_t guardCheckedSequence_ = 0;
    uint64_t guardIntervalMs_ = 5000;
    std::function<void()> cycleHookAfterSnapshot_;
    std::function<void()> rollbackHookForTesting_;
    std::atomic<bool> stopping_{true};
    std::thread thread_;
    mutable std::mutex statusMutex_;
    std::string status_ = "未启用";
};

}  // namespace ets2
