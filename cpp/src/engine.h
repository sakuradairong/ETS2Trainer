// engine.h —— 发动机 / 变速箱附属总成数据的只读探针与（后续）动力调节
//
// 偏移来源：eurotrucks2.exe 1.61.1.1 内建的字段描述表（.rdata 中「名称 → 偏移」记录），
// 由 tools/dev/dump_fields.ps1 静态导出，不依赖任何内存扫描或第三方表。
#pragma once

#include "memory.h"
#include "common.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ets2 {

// accessory_engine_data
namespace engine_field {
constexpr uint64_t kTorque = 0x1CC;
constexpr uint64_t kPower = 0x1D0;
constexpr uint64_t kTorqueCurve = 0x1F8;
constexpr uint64_t kSecondaryTorque = 0x1FC;
constexpr uint64_t kSecondaryPower = 0x200;
constexpr uint64_t kSecondaryTorqueCurve = 0x228;
constexpr uint64_t kSecondaryTorqueGearRatio = 0x22C;
constexpr uint64_t kResistanceTorque = 0x230;
constexpr uint64_t kRpmIdle = 0x234;
constexpr uint64_t kRpmLimit = 0x238;
constexpr uint64_t kRpmLimitHysteresis = 0x23C;
constexpr uint64_t kRpmLimitNeutral = 0x240;
constexpr uint64_t kRpmLimitNeutralHysteresis = 0x244;
constexpr uint64_t kRpmRangeEngineBrake = 0x24C;
constexpr uint64_t kRpmRangeLowGear = 0x254;
constexpr uint64_t kRpmRangeHighGear = 0x25C;
}  // namespace engine_field

// accessory_transmission_data
namespace transmission_field {
constexpr uint64_t kStallTorqueRatio = 0x1C4;
constexpr uint64_t kDifferentialRatio = 0x1C8;
constexpr uint64_t kShiftTime = 0x1CC;
constexpr uint64_t kRetarder = 0x1D0;
constexpr uint64_t kRatiosReverse = 0x1F8;
constexpr uint64_t kAuxiliaryBrakes = 0x220;
constexpr uint64_t kRatiosForward = 0x248;
constexpr uint64_t kCrawls = 0x24C;
constexpr uint64_t kReverseCrawls = 0x250;
constexpr uint64_t kTorqueConverterLockGear = 0x258;
constexpr uint64_t kTransmissionClass = 0x260;
constexpr uint64_t kTorqueConverterCrawl = 0x268;
constexpr uint64_t kAutoModeName = 0x288;
}  // namespace transmission_field

// 父容器中的一个指针槽（用于观察同一批附属总成数据对象）。
struct FamilyEntry {
    uint64_t slot = 0;      // 指针槽本身在进程里的地址
    uint64_t target = 0;    // 槽里保存的地址
    std::string name;       // 目标对象里找到的名称字符串（若有）
    std::vector<std::pair<uint64_t, float>> nums;  // 目标对象 0x1B0..0x270 的浮点
};

// ---------------------------------------------------------------------------
// 运行时布局：以下偏移来自 1.61.1.1 实测（数值与 .sii 定义逐项吻合）。
// 说明：描述表给出的是「定义结构」偏移，实际运行对象在中前段相差 4 字节，
//       因此这里只保留已经用数值验证过的偏移。
// ---------------------------------------------------------------------------
namespace engine_runtime {
constexpr uint64_t kTorque = 0x1C8;            // 峰值扭矩 N·m（D17A780 实测 3800）
constexpr uint64_t kResistanceTorque = 0x22C;  // 实测 346
constexpr uint64_t kRpmIdle = 0x230;           // 实测 500
constexpr uint64_t kRpmLimit = 0x234;          // 实测 2000
constexpr uint64_t kRpmLimitNeutral = 0x23C;   // 实测 2000
constexpr uint64_t kRpmRangeEngineBrake = 0x244;
constexpr uint64_t kRpmRangeLowGear = 0x24C;
constexpr uint64_t kRpmRangeHighGear = 0x254;
constexpr uint64_t kRpmRangePower = 0x25C;
constexpr uint64_t kTorqueCurveTail = 0x264;
constexpr uint64_t kLayoutBegin = 0x1B0;
constexpr uint64_t kLayoutEnd = 0x340;
}  // namespace engine_runtime

namespace transmission_runtime {
constexpr uint64_t kDifferentialRatio = 0x1C4;  // I-Shift 实测 3.08
constexpr uint64_t kStallTorqueRatio = 0x1C8;   // 无液力变矩器时实测 1.0
}  // namespace transmission_runtime

// 玩家当前车辆上下文里“附属总成数据容器”的指针偏移（实测 context -> +0x150）。
constexpr uint64_t kContextToAccessoryData = 0x150;

struct EngineTarget {
    uint64_t container = 0;         // 附属总成数据容器
    uint64_t engineData = 0;        // 发动机数据对象
    uint64_t transmissionData = 0;  // 变速箱数据对象（可能为 0）

    float torque = 0.0f;
    float torqueCopyA = 0.0f;  // +0x264（与峰值扭矩同量级的副本）
    float torqueCopyB = 0.0f;  // +0x268
    float resistanceTorque = 0.0f;
    float rpmIdle = 0.0f;
    float rpmLimit = 0.0f;
    float rpmLimitNeutral = 0.0f;
    float differentialRatio = 0.0f;
    float stallTorqueRatio = 0.0f;

    int engineMatches = 0;  // 容器内符合发动机签名的对象数量（必须为 1）
    std::vector<std::pair<uint64_t, float>> layout;  // 发动机数据对象的非零浮点布局
};

// 从玩家上下文定位发动机数据对象：只读容器的一小段并做字段签名校验。
// 命中数不为 1 时返回 false 并给出原因，绝不猜测。
bool locateEngineTarget(const ProcessMemory& memory, uint64_t context, EngineTarget* target,
                        std::string* error);

// ---------------------------------------------------------------------------
// 内存与定位抽象：生产环境包装 ProcessMemory；测试用内存镜像实现（不依赖真实游戏）。
// ---------------------------------------------------------------------------
class EngineMemory {
public:
    virtual ~EngineMemory() = default;
    virtual bool usable() const = 0;
    virtual bool readBytes(uint64_t address, void* dst, size_t size) const = 0;
    virtual bool writeBytes(uint64_t address, const void* src, size_t size) = 0;
};

class ProcessMemoryAdapter final : public EngineMemory {
public:
    explicit ProcessMemoryAdapter(ProcessMemory* memory) : memory_(memory) {}
    bool usable() const override { return memory_ != nullptr && memory_->isOpen(); }
    bool readBytes(uint64_t address, void* dst, size_t size) const override;
    bool writeBytes(uint64_t address, const void* src, size_t size) override;

private:
    ProcessMemory* memory_ = nullptr;
};

class EngineTargetResolver {
public:
    virtual ~EngineTargetResolver() = default;
    // 返回当前玩家车辆的发动机数据对象地址；无法唯一确定时必须返回 false。
    virtual bool resolve(uint64_t* engineObject, std::string* error) = 0;
};

// 生产实现：固定版本指纹 + 4 级指针链 + 附属总成容器内唯一签名命中。
class GameEngineResolver final : public EngineTargetResolver {
public:
    GameEngineResolver(ProcessMemory* memory, DWORD pid) : memory_(memory), pid_(pid) {}
    bool resolve(uint64_t* engineObject, std::string* error) override;

private:
    ProcessMemory* memory_ = nullptr;
    DWORD pid_ = 0;
};

// 发动机数据对象里的相关字段（原厂或已放大，取决于当前内存内容）。
struct EngineFieldValues {
    float torque = 0.0f;
    float copyA = 0.0f;
    float copyB = 0.0f;
    float rpmIdle = 0.0f;
    float rpmLimit = 0.0f;
    float rpmLimitNeutral = 0.0f;
    float resistance = 0.0f;
};

bool readEngineValues(const EngineMemory& memory, uint64_t object, EngineFieldValues* out);
// 定位阶段的宽松签名：允许已经被本程序放大过的扭矩（因此上限远高于原厂）。
bool engineValuesPlausible(const EngineFieldValues& values);

// ---------------------------------------------------------------------------
// 动力调节。
//
// 线程模型：所有配置、基准数据、对象地址和写入流程都由 mutex_ 保护；后台线程只调用
// maintenance()，界面线程调用 apply()/release()/restoreAndDetach()。没有裸共享状态。
//
// 基准规则：每个发动机对象地址独立保存「原厂基准」与「程序最后写入值」。
// 只有在当前值既不是最后写入值、也不是原厂基准时才重新取基准，因此不会把自己写过的
// 放大值当成原厂值再次放大。
// ---------------------------------------------------------------------------
class EngineTuner {
public:
    // 返回 false 时禁止写入；reason 会显示给用户。回调在 mutex_ 内执行，
    // 不得再次调用 EngineTuner 成员（避免重入死锁），必须尽快返回。
    using Guard = std::function<bool(std::string* reason)>;

    EngineTuner();
    ~EngineTuner();

    // 绑定内存与定位器并启动后台保持线程；重复调用会先释放旧绑定（不写内存）。
    void attach(std::unique_ptr<EngineMemory> memory, std::unique_ptr<EngineTargetResolver> resolver,
                Guard guard);
    // 放弃内存引用并停止保持线程，不做任何写入（用于内存已经失效的场景）。
    void detach();
    // 尽力写回原值后再 detach（用于退出、分离、重新附加）。
    void restoreAndDetach();

    // scale: 1.0 = 原厂扭矩；raiseLimit 为真时把转速上限提高 10%。
    bool apply(float scale, bool raiseLimit, std::string* error);
    // 写回当前对象的原厂值并停止保持。
    bool release(std::string* error);
    // 单次保持/复查：重新定位、处理换车、失败即恢复。后台线程每秒调用一次。
    bool maintenance(std::string* error);

    bool active() const;
    bool bound() const;  // 是否已绑定内存（用于避免重复 attach 丢掉恢复记录）
    float scale() const;
    bool raiseLimit() const;
    uint64_t activeObject() const;
    std::string status() const;

    // 状态修订号（线程安全，可在界面线程轮询）：下列变化会自增——
    //   活动发动机对象变化 / 新对象保存了原厂基准 / 基准值更新 /
    //   恢复成功或失败状态变化（记录清除或保留）/ 待恢复记录集合变化。
    // 界面线程比较修订号，发现变化即调用 exportState() 并原子保存设置，
    // 保证维护线程自动换车后立即持久化（强退出也不会丢 B 的恢复凭据）。
    // 注意：无实质变化的周期性 maintenance() 不会自增，不会造成每秒落盘。
    uint64_t stateRevision() const;

    // 跨进程重启的叠加保护：把「上次写入值 / 当时基准值」持久化到设置文件。
    std::string exportState() const;
    void importState(const std::string& text);
    // 是否存在「已导入但尚未核对」的恢复记录（用于界面如实提示、避免虚报恢复成功）。
    bool hasUnverifiedImportedRecord() const;
    // 是否发生过「恢复记录被判定为对象已失效而丢弃」（对象数值与本程序记录完全不符）。
    // 界面据此如实显示「对象已明确失效」，而不是谎称已确认恢复。
    bool hasInvalidatedRecord() const;

    // 测试用：关闭后台线程，改为手动调用 maintenance()。
    void setBackgroundThreadEnabled(bool enabled);
    // 测试用：调整联机安全闸门的复查间隔（默认 5000 毫秒）。
    void setGuardIntervalMs(uint64_t milliseconds);
    // 测试用：读取指定对象的原厂基准（没有记录时返回 false）。
    bool baselineOf(uint64_t object, EngineFieldValues* out) const;

private:
    struct ObjectState {
        uint64_t address = 0;
        EngineFieldValues base;     // 原厂基准
        EngineFieldValues written;  // 程序最后写入值
        bool hasWritten = false;
        bool recoveryPending = false; // 部分事务/回滚未确认，禁止把残留当新基准。
        float writtenScale = 1.0f;
        bool writtenRaiseLimit = false;
        bool useCopyA = false;
        bool useCopyB = false;
    };

    enum class RestoreOutcome { Ok, NotOurs, Failed };

    bool applyLocked(float scale, bool raiseLimit, std::string* error);
    bool maintenanceLocked(std::string* error);
    RestoreOutcome restoreObjectLocked(uint64_t address, std::string* detail);
    bool restoreActiveLocked(bool strict, std::string* error);
    bool computeValuesLocked(const ObjectState& state, float scale, bool raiseLimit,
                             EngineFieldValues* out, std::string* error) const;
    bool writeValuesLocked(uint64_t address, ObjectState& state,
                           const EngineFieldValues& values, std::string* error);
    ObjectState& baselineFor(uint64_t address, const EngineFieldValues& current);
    bool identityMatches(const ObjectState& state) const;
    std::string summaryLocked() const;
    void bumpRevisionLocked();  // 需要持久化的状态变化（mutex_ 持有中调用）
    // 尚未采纳的导入记录（跨进程恢复凭据）。只有当前对象数值正好等于某条记录的
    // 「上次写入值」时才会被采纳（消费），否则原样保留并继续导出。
    struct RemoteCredential {
        EngineFieldValues base;
        EngineFieldValues written;
        float             scale = 1.0f;
        bool              raiseLimit = false;
        bool              recoveryPending = false;
    };
    void syncRemotePrimaryLocked();  // 同步旧字段（remoteBase_/remoteWritten_/valid）供既有逻辑使用
    void run();
    void setStatusLocked(const std::string& text);

    mutable std::mutex mutex_;
    std::atomic<uint64_t> stateRevision_{0};  // 界面线程轮询用（见 stateRevision()）
    EngineMemory* memory_ = nullptr;  // 由 memoryOwner_ 持有，detach 时清空
    std::unique_ptr<EngineMemory> memoryOwner_;
    std::unique_ptr<EngineTargetResolver> resolver_;
    Guard guard_;

    bool active_ = false;
    bool guardStopped_ = false;  // 只在 mutex_ 下访问，标记最近一次写入被守卫拦截
    // 恢复记录被判定为「对象已明确失效」而丢弃（对象数值与本程序记录完全不符）
    bool invalidatedRecord_ = false;
    float scale_ = 1.0f;
    bool raiseLimit_ = false;
    uint64_t activeObject_ = 0;
    std::map<uint64_t, ObjectState> objects_;

    // 上次运行留下的记录（防止跨进程重启后把放大值当原厂值）。
    // 未采纳的记录可能有多条（例如上次运行改过 A，本次又导入 B 的记录），
    // 全部保留并导出，避免自动保存把恢复凭据覆盖成空。
    std::vector<RemoteCredential> remoteCreds_;
    bool remoteRecordValid_ = false;
    bool remoteRecordActive_ = false;
    EngineFieldValues remoteWritten_;
    EngineFieldValues remoteBase_;

    std::string status_ = "未启用";
    uint64_t guardCheckedAt_ = 0;
    uint64_t guardIntervalMs_ = 5000;

    // 线程控制（单独的等待互斥量，避免与状态锁交叉）
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
    std::atomic<bool> threadStop_{false};
    std::thread thread_;
    std::atomic<bool> backgroundEnabled_{true};
};

// 一个被判定为「发动机数据」或「变速箱数据」的对象。
struct AccessoryHit {
    uint64_t object = 0;
    std::string path;  // 例如 truck+0x208 -> +0x18
    int depth = 0;

    // 发动机字段（非发动机对象时为 0）
    float torque = 0.0f;
    float power = 0.0f;
    float rpmIdle = 0.0f;
    float rpmLimit = 0.0f;
    float rpmRangeLow = 0.0f;
    float rpmRangeHigh = 0.0f;

    // 变速箱字段
    float stallTorqueRatio = 0.0f;
    float differentialRatio = 0.0f;

    // 供人工核对：对象内 0x180..0x2A0 的浮点与指针
    std::vector<std::pair<uint64_t, float>> floats;
    std::vector<std::pair<uint64_t, uint64_t>> pointers;
    std::vector<std::pair<uint64_t, std::string>> strings;  // 偏移 -> 可打印字符串
    std::vector<FamilyEntry> family;                        // 父容器的兄弟指针槽
};

// 带有可识别名称字符串的对象（例如 @@transmission_...@@ 或 /def/vehicle/... 路径）。
struct NamedNode {
    uint64_t object = 0;
    std::string path;
    int depth = 0;
    std::vector<std::pair<uint64_t, std::string>> strings;
    std::vector<std::pair<uint64_t, float>> floats;
    std::vector<std::pair<uint64_t, uint64_t>> pointers;
};

struct AccessoryProbeResult {
    std::vector<AccessoryHit> engines;
    std::vector<AccessoryHit> transmissions;
    std::vector<NamedNode> named;
    size_t nodesVisited = 0;
    uint64_t bytesRead = 0;
    std::string note;
};

// 只读、有界的附属总成探针：从给定根节点出发做深度受限的指针遍历。
// 读取总量与节点数都有硬上限，不会遍历全部内存。
bool probeAccessories(const ProcessMemory& memory,
                      const std::vector<std::pair<std::string, uint64_t>>& roots,
                      AccessoryProbeResult* result, std::string* error);

}  // namespace ets2
