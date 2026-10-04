// enginetest.cpp —— EngineTuner 的离线状态机与并发测试
//
// 这些测试不接触真实游戏：EngineMemory 由内存镜像实现，定位器由脚本实现。
// 覆盖：倍率不叠加、换发动机对象互相恢复、RPM 开关立即恢复、超大扭矩、
//       写入失败回滚、跨进程重启叠加保护、对象失效/复用、联机闸门、并发。
#include "enginetest.h"

#include "engine.h"
#include "saves.h"
#include "ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace ets2 {

namespace {

constexpr uint64_t kEngineA = 0x0000000100000000ull;
constexpr uint64_t kEngineB = 0x0000000200000000ull;
constexpr uint64_t kEngineBig = 0x0000000300000000ull;

struct FakeEngineValues {
    float torque = 3800.0f;
    float copyA = 3773.4f;
    float copyB = 3800.0f;
    float rpmIdle = 500.0f;
    float rpmLimit = 2000.0f;
    float rpmLimitNeutral = 2000.0f;
    float resistance = 346.0f;
};

// 内存镜像：按地址保存 float，可注入写入失败与一次性的“写入后数值不对”。
class FakeMemory final : public EngineMemory {
public:
    bool usable() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return usable_;
    }

    bool readBytes(uint64_t address, void* dst, size_t size) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size != sizeof(float)) return false;
        const auto it = floats_.find(address);
        if (it == floats_.end()) return false;
        ::memcpy(dst, &it->second, sizeof(float));
        return true;
    }

    bool writeBytes(uint64_t address, const void* src, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size != sizeof(float)) return false;
        const auto it = floats_.find(address);
        if (it == floats_.end()) return false;
        if (address == failTrigger_) {
            failWrites_.insert(failRollbackAddress_);
            return false;
        }
        if (failWrites_.count(address)) return false;
        float value = 0.0f;
        ::memcpy(&value, src, sizeof(value));
        if (corruptOnce_.count(address)) {
            corruptOnce_.erase(address);
            value += 1000.0f;  // 模拟“写进去的数值不对”
        }
        it->second = value;
        ++writes_;
        return true;
    }

    void setUsable(bool usable) {
        std::lock_guard<std::mutex> lock(mutex_);
        usable_ = usable;
    }

    void mapEngine(uint64_t object, const FakeEngineValues& values) {
        std::lock_guard<std::mutex> lock(mutex_);
        floats_[object + engine_runtime::kTorque] = values.torque;
        floats_[object + engine_runtime::kTorqueCurveTail] = values.copyA;
        floats_[object + engine_runtime::kTorqueCurveTail + 4] = values.copyB;
        floats_[object + engine_runtime::kRpmIdle] = values.rpmIdle;
        floats_[object + engine_runtime::kRpmLimit] = values.rpmLimit;
        floats_[object + engine_runtime::kRpmLimitNeutral] = values.rpmLimitNeutral;
        floats_[object + engine_runtime::kResistanceTorque] = values.resistance;
    }

    void unmapEngine(uint64_t object) {
        std::lock_guard<std::mutex> lock(mutex_);
        floats_.erase(object + engine_runtime::kTorque);
        floats_.erase(object + engine_runtime::kTorqueCurveTail);
        floats_.erase(object + engine_runtime::kTorqueCurveTail + 4);
        floats_.erase(object + engine_runtime::kRpmIdle);
        floats_.erase(object + engine_runtime::kRpmLimit);
        floats_.erase(object + engine_runtime::kRpmLimitNeutral);
        floats_.erase(object + engine_runtime::kResistanceTorque);
        failWrites_.clear();
        corruptOnce_.clear();
    }

    void failWritesTo(uint64_t address, bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail) failWrites_.insert(address);
        else failWrites_.erase(address);
    }

    void corruptNextWrite(uint64_t address) {
        std::lock_guard<std::mutex> lock(mutex_);
        corruptOnce_.insert(address);
    }

    void failWriteAndRollback(uint64_t trigger, uint64_t rollbackAddress) {
        std::lock_guard<std::mutex> lock(mutex_);
        failTrigger_ = trigger;
        failRollbackAddress_ = rollbackAddress;
    }
    void clearInjectedFailures() {
        std::lock_guard<std::mutex> lock(mutex_);
        failTrigger_ = failRollbackAddress_ = 0;
        failWrites_.clear();
    }

    float get(uint64_t address) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = floats_.find(address);
        return it == floats_.end() ? 0.0f : it->second;
    }

    EngineFieldValues read(uint64_t object) const {
        EngineFieldValues values;
        values.torque = get(object + engine_runtime::kTorque);
        values.copyA = get(object + engine_runtime::kTorqueCurveTail);
        values.copyB = get(object + engine_runtime::kTorqueCurveTail + 4);
        values.rpmIdle = get(object + engine_runtime::kRpmIdle);
        values.rpmLimit = get(object + engine_runtime::kRpmLimit);
        values.rpmLimitNeutral = get(object + engine_runtime::kRpmLimitNeutral);
        values.resistance = get(object + engine_runtime::kResistanceTorque);
        return values;
    }

    uint64_t writes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return writes_;
    }

private:
    mutable std::mutex            mutex_;
    bool                          usable_ = true;
    std::map<uint64_t, float>     floats_;
    std::set<uint64_t>            failWrites_;
    std::set<uint64_t>            corruptOnce_;
    uint64_t                      writes_ = 0;
    uint64_t failTrigger_ = 0, failRollbackAddress_ = 0;
};

class FakeResolver final : public EngineTargetResolver {
public:
    void setObject(uint64_t object) {
        std::lock_guard<std::mutex> lock(mutex_);
        object_ = object;
    }
    void setError(const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error;
    }
    bool resolve(uint64_t* engineObject, std::string* error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_.empty()) {
            if (error) *error = error_;
            return false;
        }
        if (!object_) {
            if (error) *error = "fake: 没有可用对象";
            return false;
        }
        *engineObject = object_;
        return true;
    }

private:
    std::mutex  mutex_;
    uint64_t    object_ = 0;
    std::string error_;
};

bool closeTo(float a, float b, float tolerance = 0.5f) {
    return std::isfinite(a) && std::fabs(a - b) <= tolerance;
}

std::string dumpValues(const EngineFieldValues& v) {
    return fmt("torque=%.1f copyA=%.1f copyB=%.1f rpm=%.0f/%.0f", (double)v.torque,
               (double)v.copyA, (double)v.copyB, (double)v.rpmLimit,
               (double)v.rpmLimitNeutral);
}

// 测试台：EngineTuner + 内存镜像 + 脚本定位器
struct Rig {
    EngineTuner  tuner;
    FakeMemory*  memory = nullptr;
    FakeResolver* resolver = nullptr;

    explicit Rig(EngineTuner::Guard guard = nullptr) {
        auto mem = std::make_unique<FakeMemory>();
        auto res = std::make_unique<FakeResolver>();
        memory = mem.get();
        resolver = res.get();
        tuner.setBackgroundThreadEnabled(false);
        tuner.setGuardIntervalMs(0);
        tuner.attach(std::move(mem), std::move(res), std::move(guard));
    }
};

const FakeEngineValues kVolvo{3800.0f, 3773.4f, 3800.0f, 500.0f, 2000.0f, 2000.0f, 346.0f};
const FakeEngineValues kScania{2400.0f, 2380.0f, 2400.0f, 550.0f, 2200.0f, 2200.0f, 280.0f};

}  // namespace

std::vector<TunerTestItem> runEngineTunerTests() {
    std::vector<TunerTestItem> items;
    auto add = [&items](const std::string& name, bool ok, const std::string& detail) {
        items.push_back({name, ok, detail});
    };

    {
        const GameId previous=selectedGame(); setSelectedGame(GameId::Ats);
        {
            Rig rig;
            const FakeEngineValues car{556.0f,419.78f,436.46f,650.0f,6500.0f,3800.0f,100.0f};
            rig.memory->mapEngine(kEngineA,car); rig.resolver->setObject(kEngineA);
            std::string error;
            bool ok=rig.tuner.apply(1.25f,true,&error);
            auto values=rig.memory->read(kEngineA);
            add("ATS 汽车 DLC 高转速动力写入",ok && closeTo(values.torque,695.0f) && closeTo(values.rpmLimit,7150.0f) && closeTo(values.rpmLimitNeutral,4180.0f),error);
            rig.tuner.maintenance(&error);
            add("ATS 高转速动力保持不叠加",closeTo(rig.memory->read(kEngineA).torque,695.0f),error);
            ok=rig.tuner.release(&error); values=rig.memory->read(kEngineA);
            add("ATS 汽车原厂动力与转速恢复",ok && closeTo(values.torque,556.0f) && closeTo(values.rpmLimit,6500.0f) && closeTo(values.copyA,419.78f),error);
            rig.memory->failWritesTo(kEngineA+engine_runtime::kRpmLimitNeutral,true);
            ok=rig.tuner.apply(1.25f,true,&error); values=rig.memory->read(kEngineA);
            add("ATS 动力事务失败回滚",!ok && closeTo(values.torque,556.0f) && closeTo(values.rpmLimit,6500.0f),error);
            rig.memory->failWritesTo(kEngineA+engine_runtime::kRpmLimitNeutral,false);
            ok=rig.tuner.release(&error); values=rig.memory->read(kEngineA);
            add("ATS 故障解除后确认恢复",ok && rig.tuner.exportState().empty() && closeTo(values.torque,556.0f) && closeTo(values.rpmLimitNeutral,3800.0f),error);
        }
        setSelectedGame(previous);
    }

    // ---------- 1. 倍率反复切换不叠加 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        bool ok = true;
        std::string detail;
        const float scales[] = {1.25f, 1.5f, 2.0f, 1.25f, 2.0f, 1.5f};
        for (float scale : scales) {
            std::string error;
            if (!rig.tuner.apply(scale, false, &error)) {
                ok = false;
                detail = fmt("apply %.2f× 失败: %s", (double)scale, error.c_str());
                break;
            }
            rig.tuner.maintenance(nullptr);  // 模拟后台保持
            rig.tuner.maintenance(nullptr);
            const EngineFieldValues values = rig.memory->read(kEngineA);
            const float expected = 3800.0f * scale;
            if (!closeTo(values.torque, expected)) {
                ok = false;
                detail = fmt("倍率叠加：%.2f× 期望 %.1f，实际 %.1f", (double)scale,
                             (double)expected, (double)values.torque);
                break;
            }
            if (!closeTo(values.copyA, 3773.4f * scale) || !closeTo(values.copyB, 3800.0f * scale)) {
                ok = false;
                detail = "扭矩副本未同步缩放：" + dumpValues(values);
                break;
            }
            if (!closeTo(values.rpmLimit, 2000.0f)) {
                ok = false;
                detail = "未勾选转速上限时不应改动 rpmLimit";
                break;
            }
        }
        if (ok) {
            std::string error;
            rig.tuner.release(&error);
            const EngineFieldValues values = rig.memory->read(kEngineA);
            if (!closeTo(values.torque, 3800.0f)) {
                ok = false;
                detail = "释放后未回到原厂扭矩：" + dumpValues(values);
            } else {
                detail = fmt("6 次切换后释放回 3800 N·m（共写入 %llu 次）",
                             (unsigned long long)rig.memory->writes());
            }
        }
        add("动力倍率反复切换不叠加", ok, detail);
    }

    // ---------- 2. A → B → A 各自恢复自己的原值 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.memory->mapEngine(kEngineB, kScania);
        bool ok = true;
        std::string detail;
        rig.resolver->setObject(kEngineA);
        std::string error;
        ok = rig.tuner.apply(1.25f, false, &error);
        if (ok) {
            rig.tuner.maintenance(nullptr);
            ok = closeTo(rig.memory->read(kEngineA).torque, 4750.0f);
            detail = "发动机 A 未写入 4750";
        } else {
            detail = "A 写入失败: " + error;
        }
        if (ok) {
            rig.resolver->setObject(kEngineB);   // 换车/换发动机
            ok = rig.tuner.maintenance(&error) && rig.tuner.apply(1.25f, false, &error);
            if (!ok) detail = "切换到 B 失败: " + error;
        }
        if (ok) {
            rig.tuner.maintenance(nullptr);
            const EngineFieldValues a = rig.memory->read(kEngineA);
            const EngineFieldValues b = rig.memory->read(kEngineB);
            if (!closeTo(a.torque, 3800.0f)) {
                ok = false;
                detail = "旧发动机 A 未在切换前恢复：" + dumpValues(a);
            } else if (!closeTo(b.torque, 3000.0f)) {
                ok = false;
                detail = "发动机 B 未按自己的基准放大：" + dumpValues(b);
            }
        }
        if (ok) {
            rig.resolver->setObject(kEngineA);   // 切回 A
            ok = rig.tuner.maintenance(&error) && rig.tuner.apply(1.25f, false, &error);
            if (!ok) detail = "切回 A 失败: " + error;
        }
        if (ok) {
            rig.tuner.maintenance(nullptr);
            const EngineFieldValues a = rig.memory->read(kEngineA);
            const EngineFieldValues b = rig.memory->read(kEngineB);
            if (!closeTo(a.torque, 4750.0f)) {   // 3800×1.25，而不是 4750×1.25
                ok = false;
                detail = "切回 A 后倍率叠加：" + dumpValues(a);
            } else if (!closeTo(b.torque, 2400.0f)) {
                ok = false;
                detail = "B 未在切回后恢复原值：" + dumpValues(b);
            }
        }
        if (ok) {
            EngineFieldValues baseA, baseB;
            const bool haveBaseA = rig.tuner.baselineOf(kEngineA, &baseA);
            const bool haveBaseB = rig.tuner.baselineOf(kEngineB, &baseB);
            if (!haveBaseA || !haveBaseB || !closeTo(baseA.torque, 3800.0f) ||
                !closeTo(baseB.torque, 2400.0f)) {
                ok = false;
                detail = "原厂基准记录不正确";
            } else {
                std::string releaseError;
                rig.tuner.release(&releaseError);
                const EngineFieldValues a = rig.memory->read(kEngineA);
                if (!closeTo(a.torque, 3800.0f)) {
                    ok = false;
                    detail = "最终释放未恢复 A：" + dumpValues(a);
                } else {
                    detail = "A 3800↔4750、B 2400↔3000 均可互相恢复";
                }
            }
        }
        add("A→B→A 切换各自恢复原值", ok, detail);
    }

    // ---------- 3. 关闭转速增强立即恢复 RPM ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = rig.tuner.apply(1.25f, true, &error);
        std::string detail;
        if (!ok) {
            detail = "写入失败: " + error;
        } else {
            const EngineFieldValues raised = rig.memory->read(kEngineA);
            if (!closeTo(raised.rpmLimit, 2200.0f) || !closeTo(raised.rpmLimitNeutral, 2200.0f)) {
                ok = false;
                detail = "转速上限未提高：" + dumpValues(raised);
            }
        }
        if (ok) {
            ok = rig.tuner.apply(1.25f, false, &error);   // 取消勾选，动力倍率仍在
            if (!ok) {
                detail = "取消转速增强失败: " + error;
            } else {
                const EngineFieldValues back = rig.memory->read(kEngineA);
                if (!closeTo(back.rpmLimit, 2000.0f) || !closeTo(back.rpmLimitNeutral, 2000.0f)) {
                    ok = false;
                    detail = "取消勾选后 RPM 未立即恢复：" + dumpValues(back);
                } else if (!closeTo(back.torque, 4750.0f)) {
                    ok = false;
                    detail = "取消转速增强不应影响动力倍率：" + dumpValues(back);
                } else if (!rig.tuner.active()) {
                    ok = false;
                    detail = "动力倍率应仍在保持";
                }
            }
        }
        if (ok) {
            rig.tuner.release(&error);
            const EngineFieldValues values = rig.memory->read(kEngineA);
            if (!closeTo(values.torque, 3800.0f)) {
                ok = false;
                detail = "释放未回原值：" + dumpValues(values);
            } else {
                detail = "2000↔2200 RPM 立即切换，动力倍率不受影响";
            }
        }
        add("关闭转速增强立即恢复 RPM", ok, detail);
    }

    // ---------- 4. 放大超过 12000 N·m ----------
    {
        Rig rig;
        FakeEngineValues big = kVolvo;
        big.torque = 7000.0f;
        big.copyA = 7000.0f;
        big.copyB = 7000.0f;
        rig.memory->mapEngine(kEngineBig, big);
        rig.resolver->setObject(kEngineBig);
        std::string error;
        bool ok = rig.tuner.apply(2.0f, false, &error);
        std::string detail;
        if (!ok) {
            detail = "7000 N·m ×2 应允许写入，却被拒绝: " + error;
        } else {
            const EngineFieldValues values = rig.memory->read(kEngineBig);
            if (!closeTo(values.torque, 14000.0f, 1.0f)) {
                ok = false;
                detail = fmt("14000 未写入：%.1f", (double)values.torque);
            } else {
                rig.tuner.maintenance(nullptr);   // 放大后仍要能靠已保存地址保持
                const EngineFieldValues kept = rig.memory->read(kEngineBig);
                if (!closeTo(kept.torque, 14000.0f, 1.0f)) {
                    ok = false;
                    detail = "放大后保持失败（签名放宽前会在这里失败）";
                } else if (!rig.tuner.release(&error)) {
                    ok = false;
                    detail = "放大后无法恢复原值: " + error;
                } else if (!closeTo(rig.memory->read(kEngineBig).torque, 7000.0f, 1.0f)) {
                    ok = false;
                    detail = "恢复后数值不正确";
                } else {
                    detail = "7000→14000 可写入、可保持、可恢复";
                }
            }
        }
        if (ok) {
            FakeEngineValues huge = kVolvo;
            huge.torque = 20000.0f;
            huge.copyA = 20000.0f;
            huge.copyB = 20000.0f;
            rig.memory->mapEngine(kEngineB, huge);
            rig.resolver->setObject(kEngineB);
            std::string secondError;
            const bool rejected = !rig.tuner.apply(2.0f, false, &secondError);
            const EngineFieldValues values = rig.memory->read(kEngineB);
            if (!rejected) {
                ok = false;
                detail = "40000 N·m 应被安全范围拒绝";
            } else if (!closeTo(values.torque, 20000.0f, 0.5f)) {
                ok = false;
                detail = "被拒绝时不应改动内存：" + dumpValues(values);
            } else {
                detail += "；40000 N·m 被拒绝且未写入";
            }
        }
        add("超大扭矩：14000 可恢复 / 40000 拒绝", ok, detail);
    }

    // ---------- 5. 写入失败与回读失败都要回滚 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = true;
        std::string detail;
        rig.memory->failWritesTo(kEngineA + engine_runtime::kTorqueCurveTail + 4, true);
        if (rig.tuner.apply(1.25f, false, &error)) {
            ok = false;
            detail = "副本写入失败时不应报告成功";
        } else {
            const EngineFieldValues values = rig.memory->read(kEngineA);
            if (!closeTo(values.torque, 3800.0f) || !closeTo(values.copyA, 3773.4f) ||
                !closeTo(values.copyB, 3800.0f) || !closeTo(values.rpmLimit, 2000.0f)) {
                ok = false;
                detail = "部分写入失败后未回滚：" + dumpValues(values);
            } else if (error.find("回滚") == std::string::npos && error.find("失败") == std::string::npos) {
                ok = false;
                detail = "错误信息未说明失败原因: " + error;
            } else {
                detail = "副本写入失败 → 已回滚（" + error + "）";
            }
        }
        if (ok) {
            rig.memory->failWritesTo(kEngineA + engine_runtime::kTorqueCurveTail + 4, false);
            rig.memory->corruptNextWrite(kEngineA + engine_runtime::kTorque);
            std::string secondError;
            if (rig.tuner.apply(1.5f, false, &secondError)) {
                ok = false;
                detail = "回读数值不符时不应报告成功";
            } else {
                const EngineFieldValues values = rig.memory->read(kEngineA);
                if (!closeTo(values.torque, 3800.0f)) {
                    ok = false;
                    detail = "回读失败后未回滚：" + dumpValues(values);
                } else if (secondError.find("回读") == std::string::npos) {
                    ok = false;
                    detail = "错误信息未指出回读失败: " + secondError;
                } else {
                    detail += "；回读失败同样回滚";
                }
            }
        }
        add("写入/回读失败时回滚", ok, detail);
    }

    // ---------- 6. 跨进程重启的叠加保护 ----------
    {
        std::string persisted;
        {
            Rig first;
            first.memory->mapEngine(kEngineA, kVolvo);
            first.resolver->setObject(kEngineA);
            std::string error;
            if (first.tuner.apply(1.25f, false, &error)) {
                persisted = first.tuner.exportState();
            }
            // 模拟“程序退出但游戏里仍是放大值”：旧对象被释放，地址重新映射成 4750
        }
        Rig second;
        FakeEngineValues amplified = kVolvo;
        amplified.torque = 4750.0f;
        amplified.copyA = 4716.75f;
        amplified.copyB = 4750.0f;
        second.memory->mapEngine(kEngineA, amplified);
        second.resolver->setObject(kEngineA);
        second.tuner.importState(persisted);
        std::string error;
        bool ok = !persisted.empty() && second.tuner.apply(1.25f, false, &error);
        std::string detail;
        if (!ok) {
            detail = "上一次运行记录无效或写入失败: " + error;
        } else {
            const EngineFieldValues values = second.memory->read(kEngineA);
            if (!closeTo(values.torque, 4750.0f, 1.0f)) {
                ok = false;
                detail = fmt("把放大值当成了原厂基准（实际写入 %.1f，应为 4750）",
                             (double)values.torque);
            } else {
                second.tuner.release(&error);
                const EngineFieldValues back = second.memory->read(kEngineA);
                if (!closeTo(back.torque, 3800.0f, 1.0f)) {
                    ok = false;
                    detail = "无法用上次运行的原厂基准恢复（应为 3800）";
                } else {
                    detail = "跨重启仍是 3800 基准，恢复到 3800 N·m";
                }
            }
        }
        add("跨进程重启不叠加（记录上次写入值）", ok, detail);
    }

    // ---------- 7. 对象失效 / 地址被复用 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = rig.tuner.apply(1.25f, false, &error);
        std::string detail;
        if (!ok) {
            detail = "初始写入失败: " + error;
        } else {
            rig.memory->unmapEngine(kEngineA);            // 对象被释放
            rig.tuner.maintenance(nullptr);               // 不应崩溃
            FakeEngineValues other = kVolvo;              // 地址被别的发动机复用
            other.torque = 9999.0f;
            other.copyA = 9999.0f;
            other.copyB = 9999.0f;
            other.rpmIdle = 600.0f;
            other.rpmLimit = 2100.0f;
            other.rpmLimitNeutral = 2100.0f;
            rig.memory->mapEngine(kEngineA, other);
            rig.tuner.maintenance(nullptr);
            ok = rig.tuner.apply(1.25f, false, &error);
            if (!ok) {
                detail = "复用后重新应用失败: " + error;
            } else {
                const EngineFieldValues values = rig.memory->read(kEngineA);
                if (!closeTo(values.torque, 12498.75f, 1.0f)) {
                    ok = false;
                    detail = fmt("地址复用后应以新对象原值为基准（期望 12498.75，实际 %.1f）",
                                 (double)values.torque);
                } else {
                    rig.tuner.release(&error);
                    if (!closeTo(rig.memory->read(kEngineA).torque, 9999.0f, 1.0f)) {
                        ok = false;
                        detail = "释放后未回到新对象原值 9999";
                    } else {
                        detail = "对象失效不崩溃，地址复用后以新原值 9999 为基准";
                    }
                }
            }
        }
        add("对象失效 / 地址复用安全处理", ok, detail);
    }

    // ---------- 8. 联机闸门：启用前与运行中都复查 ----------
    {
        auto blocked = std::make_shared<std::atomic<bool>>(false);
        Rig rig([blocked](std::string* reason) {
            if (blocked->load()) {
                if (reason) *reason = "检测到官方 Convoy 会话";
                return false;
            }
            return true;
        });
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = rig.tuner.apply(1.25f, false, &error);
        std::string detail;
        if (!ok) {
            detail = "单机状态写入失败: " + error;
        } else if (!closeTo(rig.memory->read(kEngineA).torque, 4750.0f)) {
            ok = false;
            detail = "单机写入数值不正确";
        } else {
            blocked->store(true);              // 运行中进入联机
            rig.tuner.maintenance(nullptr);
            const EngineFieldValues values = rig.memory->read(kEngineA);
            if (!closeTo(values.torque, 3800.0f)) {
                ok = false;
                detail = "进入联机后没有自动恢复原值：" + dumpValues(values);
            } else if (rig.tuner.active()) {
                ok = false;
                detail = "进入联机后仍在保持修改";
            } else if (rig.tuner.status().find("Convoy") == std::string::npos) {
                ok = false;
                detail = "状态未说明被联机闸门拦下: " + rig.tuner.status();
            } else {
                blocked->store(false);
                std::string secondError;
                if (!rig.tuner.apply(1.25f, false, &secondError)) {
                    ok = false;
                    detail = "退出联机后无法重新启用: " + secondError;
                } else {
                    detail = "进入联机自动恢复原值，并停止保持";
                }
            }
        }
        add("联机闸门（启用前 + 运行中复查）", ok, detail);
    }

    // ---------- 9. UI 快速切换的并发安全 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::atomic<bool> stop{false};
        std::atomic<int>  failures{0};
        const float scales[] = {1.0f, 1.25f, 1.5f, 2.0f};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&rig, &stop, &failures, &scales, t]() {
                int index = t;
                for (int i = 0; i < 200; ++i) {
                    const float scale = scales[index++ % 4];
                    std::string error;
                    if (scale == 1.0f) {
                        rig.tuner.release(&error);
                    } else if (!rig.tuner.apply(scale, (i % 2) == 0, &error)) {
                        ++failures;
                    }
                    if ((i % 3) == 0) rig.tuner.maintenance(nullptr);
                }
                (void)stop;
            });
        }
        for (auto& thread : threads) thread.join();
        std::string error;
        const bool released = rig.tuner.release(&error);
        const EngineFieldValues values = rig.memory->read(kEngineA);
        const bool ok = released && failures.load() == 0 && closeTo(values.torque, 3800.0f) &&
                        closeTo(values.copyA, 3773.4f) && closeTo(values.copyB, 3800.0f) &&
                        closeTo(values.rpmLimit, 2000.0f) && closeTo(values.rpmLimitNeutral, 2000.0f);
        add("4 线程 × 200 次快速切换无竞争",
            ok, ok ? fmt("最终回到原厂值（写入 %llu 次）",
                         (unsigned long long)rig.memory->writes())
                   : fmt("失败 %d 次，最终 %s", failures.load(), dumpValues(values).c_str()));
    }

    // ---------- 10. 内存失效（游戏退出）时不写、不崩 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = rig.tuner.apply(1.25f, false, &error);
        std::string detail;
        if (!ok) {
            detail = "初始写入失败: " + error;
        } else {
            const uint64_t writesBefore = rig.memory->writes();
            rig.memory->setUsable(false);   // 模拟游戏进程关闭
            rig.tuner.maintenance(nullptr);
            const bool stopped = !rig.tuner.active();
            const bool explained = rig.tuner.status().find("进程已关闭") != std::string::npos;
            const bool noFurtherWrites = rig.memory->writes() == writesBefore;
            ok = stopped && explained && noFurtherWrites;
            detail = ok ? "进程失效后停止保持、给出提示且不再写入"
                        : ("状态异常: " + rig.tuner.status());
        }
        rig.tuner.restoreAndDetach();  // 不应崩溃
        add("进程失效后停止且不崩溃", ok, detail);
    }

    // ---------- 11. 分离还原失败后必须保留跨重启恢复记录 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = rig.tuner.apply(1.25f, false, &error);
        if (ok) {
            rig.memory->failWritesTo(kEngineA + engine_runtime::kTorque, true);
            rig.tuner.restoreAndDetach();
            const std::string record = rig.tuner.exportState();
            ok = !record.empty() && rig.tuner.status().find("原值未恢复") != std::string::npos;
        }
        add("还原失败后仍可导出原厂基准", ok,
            ok ? "恢复记录保留，下一次启动可核对基准" : "恢复失败时记录丢失：" + error);
    }

    // ---------- 12. 守卫命中且写回失败：必须诚实报告并保留记录 ----------
    {
        bool block = false;
        Rig rig([&block](std::string* reason) {
            if (block && reason) *reason = "测试联机";
            return !block;
        });
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = rig.tuner.apply(1.25f, false, &error);
        if (ok) {
            rig.memory->failWritesTo(kEngineA + engine_runtime::kTorque, true);
            block = true;
            rig.tuner.maintenance(&error);
            const std::string status = rig.tuner.status();
            ok = !rig.tuner.active() && status.find("未能恢复") != std::string::npos &&
                 !rig.tuner.exportState().empty();
        }
        add("联机守卫恢复失败真实报告", ok,
            ok ? "状态保留恢复失败详情且记录仍可导出" : "错误状态：" + rig.tuner.status());
    }

    // ---------- 13. 状态修订号：后台换车自增、无变化不自增 ----------
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.memory->mapEngine(kEngineB, kScania);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = rig.tuner.apply(1.25f, false, &error);
        std::string detail;
        uint64_t revSteady = 0;
        if (ok) {
            const uint64_t rev0 = rig.tuner.stateRevision();
            rig.tuner.maintenance(nullptr);   // 同对象保持：不应有需要持久化的变化
            rig.tuner.maintenance(nullptr);
            revSteady = rig.tuner.stateRevision();
            if (revSteady != rev0) {
                ok = false;
                detail = "无变化的保持周期也自增修订号（会造成每秒落盘）";
            }
        }
        if (ok) {
            rig.resolver->setObject(kEngineB);   // 后台自动换车
            ok = rig.tuner.maintenance(&error);
            if (ok) {
                const uint64_t revAfterSwitch = rig.tuner.stateRevision();
                if (revAfterSwitch == revSteady) {
                    ok = false;
                    detail = "自动换车后修订号没有自增（UI 不会持久化 B 的记录）";
                } else {
                    detail = fmt("保持期 %llu → 换车后 %llu（UI 会触发一次落盘）",
                                 (unsigned long long)revSteady,
                                 (unsigned long long)revAfterSwitch);
                }
            } else {
                detail = "换车 maintenance 失败: " + error;
            }
        }
        add("状态修订号：换车自增、平稳保持不自增", ok, detail);
    }

    // ---------- 14. 应用 A → 自动切换 B → 强制退出 → 重导入：恢复 B 且不叠加 ----------
    {
        std::string persisted;
        {
            Rig first;
            first.memory->mapEngine(kEngineA, kVolvo);
            first.memory->mapEngine(kEngineB, kScania);
            first.resolver->setObject(kEngineA);
            std::string error;
            bool ok = first.tuner.apply(1.25f, false, &error);
            if (ok) {
                first.resolver->setObject(kEngineB);   // 维护线程发现换车
                ok = first.tuner.maintenance(&error);
            }
            if (ok) {
                // UI 在修订号变化后调用 exportState —— 必须已描述 B 的记录
                persisted = first.tuner.exportState();
                ok = !persisted.empty();
            }
            // 析构会尝试恢复；第二个测试台用新的内存镜像模拟
            // 「程序强退后游戏里仍留着 B 的放大值」。
        }
        Rig second;
        FakeEngineValues amplifiedB = kScania;   // 2400 × 1.25 = 3000
        amplifiedB.torque = 3000.0f;
        amplifiedB.copyA = 2975.0f;
        amplifiedB.copyB = 3000.0f;
        second.memory->mapEngine(kEngineB, amplifiedB);
        second.resolver->setObject(kEngineB);
        second.tuner.importState(persisted);
        std::string error;
        bool ok = !persisted.empty() && second.tuner.apply(1.25f, false, &error);
        std::string detail;
        if (!ok) {
            detail = "重导入后应用失败: " + error;
        } else {
            const EngineFieldValues values = second.memory->read(kEngineB);
            if (!closeTo(values.torque, 3000.0f, 1.0f)) {
                ok = false;
                detail = fmt("倍率叠加：期望 3000（2400×1.25），实际 %.1f",
                             (double)values.torque);
            } else {
                std::string releaseError;
                second.tuner.release(&releaseError);
                const EngineFieldValues back = second.memory->read(kEngineB);
                if (!closeTo(back.torque, 2400.0f, 1.0f)) {
                    ok = false;
                    detail = fmt("无法恢复 B 的原厂值：期望 2400，实际 %.1f",
                                 (double)back.torque);
                } else {
                    detail = "强退重导后仍以 2400 为基准：写入 3000、恢复 2400";
                }
            }
        }
        add("换车后持久化：强退重导恢复 B 且不叠加", ok, detail);
    }

    // ---------- 15. 设置持久化：往返一致 / 旧格式兼容 / 失败不清恢复记录 ----------
    {
        const std::wstring dir = applicationDir();
        const std::wstring path = dir + L"\\settings_selftest.ini";
        const std::wstring legacyPath = dir + L"\\settings_selftest_legacy.ini";
        const std::wstring badPath = dir + L"\\__no_such_dir__\\settings.ini";
        ::DeleteFileW(path.c_str());
        ::DeleteFileW(legacyPath.c_str());

        bool ok = true;
        std::string detail;

        // 1) 往返一致：序列化 -> 落盘 -> 读回
        AppState app;
        ::strncpy_s(app.moneyTarget, sizeof(app.moneyTarget), "424242", _TRUNCATE);
        ::strncpy_s(app.xpTarget, sizeof(app.xpTarget), "7777", _TRUNCATE);
        app.workers = 6;
        app.engineStateText = "1;3800.0000;3773.4000;3800.0000;2000.0000;2000.0000;"
                              "4750.0000;4716.7500;4750.0000;2000.0000;2000.0000;1.250;0";
        if (!saveSettingsTo(app, path)) {
            ok = false;
            detail = "原子保存失败: " + W2U(path);
        }
        AppState loaded;
        loadSettingsFrom(loaded, path);
        if (ok) {
            ok = std::string(loaded.moneyTarget) == "424242" &&
                 std::string(loaded.xpTarget) == "7777" && loaded.workers == 6 &&
                 loaded.engineStateText == app.engineStateText;
            if (!ok) detail = "设置往返不一致";
        }
        if (ok) {
            // 原子写不得留下临时文件
            const bool noTemp =
                ::GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES;
            if (!noTemp) {
                ok = false;
                detail = "原子保存遗留 .tmp 文件";
            }
        }

        // 2) 旧格式兼容：只有部分键、CRLF 行尾的旧 settings.ini 仍能读取
        if (ok) {
            const std::string legacy =
                "money_type=int64\r\nmoney_target=12345\r\n"
                "engine_state=1;2400;0;0;2200;2200;3000;0;0;2200;2200;1.250;0\r\n";
            if (!writeTextFileAtomic(legacyPath, legacy)) {
                ok = false;
                detail = "写入旧格式样例失败";
            } else {
                AppState legacyApp;
                loadSettingsFrom(legacyApp, legacyPath);
                ok = std::string(legacyApp.moneyTarget) == "12345" &&
                     legacyApp.engineStateText.find("1;2400;") == 0;
                if (!ok) detail = "旧格式 settings.ini 读取不兼容";
            }
        }

        // 3) 持久化失败不得清除恢复记录（内存中的 engine_state / tuner 记录都保留）
        if (ok) {
            AppState failing;
            failing.engineTuner = std::make_unique<EngineTuner>();
            failing.engineTuner->setBackgroundThreadEnabled(false);
            auto memory = std::make_unique<FakeMemory>();
            auto resolver = std::make_unique<FakeResolver>();
            FakeMemory* rawMemory = memory.get();
            FakeResolver* rawResolver = resolver.get();
            rawMemory->mapEngine(kEngineA, kVolvo);
            rawResolver->setObject(kEngineA);
            failing.engineTuner->attach(std::move(memory), std::move(resolver), nullptr);
            std::string error;
            const bool applied = failing.engineTuner->apply(1.25f, false, &error);
            const std::string record = failing.engineTuner->exportState();
            const bool saveFailed = !saveSettingsTo(failing, badPath);  // 目标目录不存在
            const std::string recordAfter = failing.engineTuner->exportState();
            ok = applied && saveFailed && !record.empty() && recordAfter == record;
            if (!ok) {
                detail = fmt("applied=%d saveFailed=%d 记录前=%d 后=%d", (int)applied,
                             (int)saveFailed, (int)!record.empty(), (int)recordAfter.size());
            } else {
                detail = "往返一致、旧格式兼容、无 .tmp 残留、失败时恢复记录保留";
            }
        }

        if (ok && detail.empty()) detail = "序列化往返 / 旧格式 / 原子写 / 失败保留 全部通过";
        ::DeleteFileW(path.c_str());
        ::DeleteFileW(legacyPath.c_str());
        add("设置持久化：往返一致 + 旧格式兼容 + 失败不清记录", ok, detail);
    }

    // ---------- 16. 审查问题 3：尚未采纳的导入记录不得被导出为空 ----------
    {
        const std::string saved =
            "1;3800.0000;3773.4000;3800.0000;2000.0000;2000.0000;"
            "4750.0000;4716.7500;4750.0000;2000.0000;2000.0000;1.250;0";
        EngineTuner tuner;
        tuner.setBackgroundThreadEnabled(false);
        tuner.importState(saved);
        const std::string exported = tuner.exportState();
        AppState app;
        app.engineTuner = std::make_unique<EngineTuner>();
        app.engineTuner->setBackgroundThreadEnabled(false);
        app.engineTuner->importState(saved);
        app.engineTuner.reset();  // 只用 engineStateText 序列化路径
        app.engineStateText = exported;
        const std::string serialized = settingsText(app);
        // 关键：导入后立即导出/序列化，恢复凭据必须还在（旧实现导出为空 → 被自动保存抹掉）
        const bool kept = !exported.empty() &&
                          serialized.find("engine_state=\n") == std::string::npos &&
                          serialized.find("engine_state=1;") != std::string::npos;
        // 重新加载后凭据依然可用：应用同一个放大值时不得再乘一次倍率
        EngineTuner reloaded;
        reloaded.setBackgroundThreadEnabled(false);
        reloaded.importState(exported);
        Rig rig;
        rig.memory->mapEngine(kEngineA, [] {
            FakeEngineValues amplified = kVolvo;
            amplified.torque = 4750.0f;
            amplified.copyA = 4716.75f;
            amplified.copyB = 4750.0f;
            return amplified;
        }());
        rig.resolver->setObject(kEngineA);
        rig.tuner.importState(exported);
        std::string error;
        const bool applied = rig.tuner.apply(1.25f, false, &error);
        const EngineFieldValues values = rig.memory->read(kEngineA);
        const bool noStacking = applied && closeTo(values.torque, 4750.0f, 1.0f);
        const bool reloadOk = reloaded.hasUnverifiedImportedRecord() &&
                              !reloaded.exportState().empty();
        bool ok = kept && noStacking && reloadOk;
        std::string detail = ok ? "导入→导出/序列化→重载 凭据保留，且不叠加倍率"
                                : fmt("kept=%d 导出字节=%d noStacking=%d 实际=%.1f", (int)kept,
                                      (int)exported.size(), (int)noStacking,
                                      (double)values.torque);
        // 恢复成功（核对完成）后必须清空，不能留下过期凭据
        std::string releaseError;
        const bool released = rig.tuner.release(&releaseError);
        const bool backToStock = closeTo(rig.memory->read(kEngineA).torque, 3800.0f, 1.0f);
        if (ok && (!released || !backToStock || !rig.tuner.exportState().empty())) {
            ok = false;
            detail = fmt("核对/恢复后未清空过期凭据或未回原厂（released=%d torque=%.1f）",
                         (int)released, (double)rig.memory->read(kEngineA).torque);
        }
        add("导入的恢复记录在核对前不会被导出为空", ok, detail);
    }

    // ---------- 17. 审查问题 3：首次应用失败→保存→重启→成功核对，不叠加倍率；不虚报恢复 ----------
    {
        const std::string saved =
            "1;2400.0000;2380.0000;2400.0000;2200.0000;2200.0000;"
            "3000.0000;2975.0000;3000.0000;2200.0000;2200.0000;1.250;0";
        std::string exported;
        {
            EngineTuner tuner;
            tuner.setBackgroundThreadEnabled(false);
            tuner.importState(saved);
            exported = tuner.exportState();
            // 定位失败（无对象可绑定）：导出必须仍然保留凭据
        }
        Rig rig;  // 新一次运行：游戏里仍是 B 的放大值 3000
        FakeEngineValues amplified = kScania;
        amplified.torque = 3000.0f;
        amplified.copyA = 2975.0f;
        amplified.copyB = 3000.0f;
        rig.memory->mapEngine(kEngineB, amplified);
        rig.resolver->setObject(kEngineB);
        rig.tuner.importState(exported);
        // 尚未核对时「恢复原厂」不得虚报成功
        std::string honestError;
        const bool falseSuccess = rig.tuner.release(&honestError);
        const bool honest = !falseSuccess && !honestError.empty() &&
                            rig.tuner.hasUnverifiedImportedRecord() &&
                            !rig.tuner.exportState().empty();
        std::string error;
        const bool applied = rig.tuner.apply(1.25f, false, &error);
        const EngineFieldValues values = rig.memory->read(kEngineB);
        const bool noStacking = applied && closeTo(values.torque, 3000.0f, 1.0f);
        std::string releaseError;
        rig.tuner.release(&releaseError);
        const bool restored = closeTo(rig.memory->read(kEngineB).torque, 2400.0f, 1.0f);
        const bool cleared = rig.tuner.exportState().empty() &&
                             !rig.tuner.hasUnverifiedImportedRecord();
        const bool ok = honest && noStacking && restored && cleared;
        add("导入记录：失败后保留 / 不虚报恢复 / 成功核对后不叠加", ok,
            ok ? "尚未核对时不报成功，核对后 2400 基准、恢复后凭据清空"
               : fmt("honest=%d noStacking=%d(实际 %.1f) restored=%d cleared=%d", (int)honest,
                     (int)noStacking, (double)values.torque, (int)restored, (int)cleared));
    }

    // ---------- 18. 审查问题 8：经验目标值范围校验（先判范围再转换） ----------
    {
        int32_t value = 0;
        std::string error;
        bool ok = true;
        std::string detail;
        // 合法边界
        if (!parseXpTarget("0", &value, &error) || value != 0) {
            ok = false;
            detail = "0 被拒绝";
        }
        // 业务上限内
        if (ok && (!parseXpTarget("2000000000", &value, &error) || value != 2000000000)) {
            ok = false;
            detail = "业务上限 2e9 被拒绝";
        }
        // int32 上界本身超出业务上限：必须以「业务上限」为由拒绝（绝不做越界转换）
        if (ok) {
            value = -12345;
            if (parseXpTarget("2147483647", &value, &error) || value != -12345) {
                ok = false;
                detail = "INT32_MAX 未被业务上限拒绝（或写出了输出值）";
            } else if (error.find("上限") == std::string::npos) {
                ok = false;
                detail = "INT32_MAX 的拒绝原因不是业务上限：" + error;
            }
        }
        // 必须拒绝：负数、业务上限外、int32 边界外、超大整数、非整数
        const char* rejected[] = {"-1",          "2000000001", "2147483648",
                                  "4294967296",  "999999999999999999999999",
                                  "1e30",        "12.5",       ""};
        for (const char* text : rejected) {
            if (!ok) break;
            if (parseXpTarget(text, &value, &error)) {
                ok = false;
                detail = std::string("越界值被接受：") + text;
            }
        }
        // 金钱同样先判范围
        int64_t money = 0;
        if (ok && (!parseMoneyTarget("1000000000", &money, &error) || money != 1000000000LL)) {
            ok = false;
            detail = "合法金额被拒绝";
        }
        if (ok && parseMoneyTarget("99999999999999999999", &money, &error)) {
            ok = false;
            detail = "超大金额被接受";
        }
        if (ok && parseMoneyTarget("not-a-number", &money, &error)) {
            ok = false;
            detail = "非数字金额被接受";
        }
        add("经验/金钱目标值：先判范围再转换（含边界与越界）", ok,
            ok ? "0/INT32_MAX/2e9 接受；负数、越界、非整数、超大值全部拒绝"
               : detail);
    }

    // ---------- 19. 审查复审 1：多条凭据（2 条/3 条）解析、往返与"对象记录+未采纳凭据"共存 ----------
    {
        // 凭据格式：flags;base5;written5;scale;raise。copyA/copyB 必须与真实放大副本一致，
        // 否则不会被采纳（这正是叠加保护按「写入值三元组」匹配的原因）。
        auto record = [](float baseTorque, float baseCopyA, float baseCopyB, float rpmLimit,
                         float writtenTorque, float writtenCopyA, float writtenCopyB,
                         const char* lead) {
            return fmt("%s;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;%.4f;"
                       "%.4f;1.250;0",
                       lead, baseTorque, baseCopyA, baseCopyB, rpmLimit, rpmLimit, writtenTorque,
                       writtenCopyA, writtenCopyB, rpmLimit, rpmLimit);
        };
        // 3800 N·m 发动机（rpm 上限 2000）：copyA 3773.4 → 4716.75
        const std::string credBig =
            record(3800.0f, 3773.4f, 3800.0f, 2000.0f, 4750.0f, 4716.75f, 4750.0f, ";P");
        // 2400 N·m 发动机（rpm 上限 2200）：copyA 2380 → 2975
        const std::string credSmall =
            record(2400.0f, 2380.0f, 2400.0f, 2200.0f, 3000.0f, 2975.0f, 3000.0f, "1");
        // 1500 N·m 发动机（仅用于 3 条解析/往返）
        const std::string credTiny =
            record(1500.0f, 1480.0f, 1500.0f, 1800.0f, 1875.0f, 1850.0f, 1875.0f, ";P");
        const std::string two = credSmall + credBig;
        const std::string three = two + credTiny;

        EngineTuner tunerA;
        tunerA.setBackgroundThreadEnabled(false);
        tunerA.importState(two);
        const std::string exportedTwo = tunerA.exportState();
        const int markersTwo = (int)std::count(exportedTwo.begin(), exportedTwo.end(), 'P');
        const bool twoOk = exportedTwo.find("3800") != std::string::npos && markersTwo == 1;

        EngineTuner tunerB;
        tunerB.setBackgroundThreadEnabled(false);
        tunerB.importState(three);
        const std::string exportedThree = tunerB.exportState();
        const int markersThree = (int)std::count(exportedThree.begin(), exportedThree.end(), 'P');
        // 往返稳定：导出→再导入→再导出，条数不变
        EngineTuner tunerC;
        tunerC.setBackgroundThreadEnabled(false);
        tunerC.importState(exportedThree);
        const std::string roundTrip = tunerC.exportState();
        const int markersRoundTrip = (int)std::count(roundTrip.begin(), roundTrip.end(), 'P');
        const bool threeOk = markersThree == 2 && markersRoundTrip == 2 &&
                             exportedThree.find("1500") != std::string::npos &&
                             roundTrip.find("3800") != std::string::npos;

        // 两辆车各按自己的真实基准恢复（2400 与 3800 两条凭据各自生效）
        Rig rig;
        FakeEngineValues amplifiedA = kVolvo;      // 3800×1.25 = 4750
        amplifiedA.torque = 4750.0f;
        amplifiedA.copyA = 4716.75f;
        amplifiedA.copyB = 4750.0f;
        FakeEngineValues amplifiedB = kScania;     // 2400×1.25 = 3000
        amplifiedB.torque = 3000.0f;
        amplifiedB.copyA = 2975.0f;
        amplifiedB.copyB = 3000.0f;
        rig.memory->mapEngine(kEngineA, amplifiedA);
        rig.memory->mapEngine(kEngineB, amplifiedB);
        rig.tuner.importState(two);
        rig.resolver->setObject(kEngineA);
        std::string error;
        const bool appliedA = rig.tuner.apply(1.25f, false, &error);
        const bool noStackA = closeTo(rig.memory->read(kEngineA).torque, 4750.0f, 1.0f);
        rig.resolver->setObject(kEngineB);
        const bool switched = rig.tuner.maintenance(&error) &&
                              rig.tuner.apply(1.25f, false, &error);
        const bool noStackB = closeTo(rig.memory->read(kEngineB).torque, 3000.0f, 1.0f);
        std::string releaseError;
        rig.tuner.release(&releaseError);
        const bool backB = closeTo(rig.memory->read(kEngineB).torque, 2400.0f, 1.0f);
        const bool backA = closeTo(rig.memory->read(kEngineA).torque, 3800.0f, 1.0f);
        const bool bothOk = appliedA && noStackA && switched && noStackB && backB && backA;

        add("发动机多条凭据：2/3 条解析与往返 + 两车各自恢复真实基准",
            twoOk && threeOk && bothOk,
            fmt("2 条=%d(标记 %d) 3 条=%d(标记 %d/往返 %d) 两车恢复=%d（appliedA=%d switch=%d "
                "noStackA=%d noStackB=%d backA=%d backB=%d；A=%.1f B=%.1f err=%s）",
                (int)twoOk, markersTwo, (int)threeOk, markersThree, markersRoundTrip,
                (int)bothOk, (int)appliedA, (int)switched, (int)noStackA, (int)noStackB,
                (int)backA, (int)backB, (double)rig.memory->read(kEngineA).torque,
                (double)rig.memory->read(kEngineB).torque, releaseError.c_str()));
    }

    // ---------- 20. 审查复审 1：已修改对象记录 与 未采纳凭据 共存时导出不丢任一 ----------
    {
        Rig rig;
        FakeEngineValues amplified = kVolvo;
        amplified.torque = 4750.0f;
        amplified.copyA = 4716.75f;
        amplified.copyB = 4750.0f;
        rig.memory->mapEngine(kEngineA, amplified);
        rig.memory->mapEngine(kEngineB, kScania);
        rig.resolver->setObject(kEngineA);
        std::string error;
        const bool applied = rig.tuner.apply(1.25f, false, &error);
        // 再导入一条「另一台发动机」的未采纳凭据（原厂 2400 → 写 3000）
        rig.tuner.importState("1;2400.0000;2380.0000;2400.0000;2200.0000;2200.0000;"
                              "3000.0000;2975.0000;3000.0000;2200.0000;2200.0000;1.250;0");
        const std::string exported = rig.tuner.exportState();
        const bool hasObjectRecord = exported.find("4750") != std::string::npos;
        const bool hasPending = exported.find(";P;") != std::string::npos &&
                                exported.find("2400") != std::string::npos;
        add("发动机凭据共存：对象记录与未采纳凭据同时导出", applied && hasObjectRecord &&
                                                                 hasPending,
            fmt("导出字节=%d 对象记录=%d 未采纳凭据=%d", (int)exported.size(),
                (int)hasObjectRecord, (int)hasPending));
    }

    {
        const std::string primary =
            "1;2400.0000;2380.0000;2400.0000;2200.0000;2200.0000;"
            "4200.0000;4165.0000;4200.0000;2420.0000;2420.0000;1.750;1";
        const std::string extra =
            ";P;3800.0000;3776.0000;3800.0000;2000.0000;2000.0000;"
            "4750.0000;4720.0000;4750.0000;2000.0000;2000.0000;1.250;0";
        bool ok = true;
        for (const std::string& state : {primary, primary + extra}) {
            std::string next = state;
            for (int round = 0; round < 3; ++round) {
                EngineTuner tuner;
                tuner.setBackgroundThreadEnabled(false);
                tuner.importState(next);
                next = tuner.exportState();
                ok = ok && next == state && closeTo(tuner.scale(), 1.0f, 0.0001f) &&
                     !tuner.raiseLimit(); // 导入凭据不会擅自启用动力/转速策略。
            }
        }
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        const bool applied = rig.tuner.apply(1.25f, false, &error);
        rig.tuner.importState(primary);
        const std::string mixed = rig.tuner.exportState();
        const std::string pending = ";P;" + primary.substr(2);
        ok = ok && applied && mixed.find("4750") != std::string::npos &&
             mixed.find(pending) != std::string::npos && closeTo(rig.tuner.scale(), 1.25f, 0.0001f) &&
             !rig.tuner.raiseLimit();
        add("发动机凭据：主/追加/共存记录保留各自倍率与转速标志", ok,
            ok ? "1.750/raise=1 与 1.250/raise=0 往返三轮保持，且不改变当前策略" : error);
    }
    // 完整事务失败后逐字段保留恢复凭据，包含首次与已有放大值的后续事务。
    for (bool previouslyApplied : {false, true}) {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        std::string error;
        bool ok = !previouslyApplied || rig.tuner.apply(1.25f, false, &error);
        rig.memory->failWriteAndRollback(kEngineA + engine_runtime::kRpmLimit,
                                        kEngineA + engine_runtime::kTorque);
        const bool failed = !rig.tuner.apply(previouslyApplied ? 1.5f : 1.25f, false, &error);
        const float residual = rig.memory->read(kEngineA).torque;
        const EngineFieldValues residualValues = rig.memory->read(kEngineA);
        const std::string saved = rig.tuner.exportState();
        EngineFieldValues baseline;
        ok = ok && failed && saved.find(";U") != std::string::npos &&
             rig.tuner.baselineOf(kEngineA, &baseline) && closeTo(baseline.torque, 3800.0f);
        EngineTuner reloaded;
        reloaded.setBackgroundThreadEnabled(false);
        reloaded.importState(saved);
        ok = ok && reloaded.exportState() == saved;
        rig.memory->clearInjectedFailures();
        const bool retried = rig.tuner.apply(1.25f, false, &error);
        ok = ok && retried && closeTo(rig.memory->read(kEngineA).torque, 4750.0f) &&
             rig.tuner.baselineOf(kEngineA, &baseline) && closeTo(baseline.torque, 3800.0f) &&
             rig.tuner.exportState().find(";U") == std::string::npos;
        const bool released = rig.tuner.release(&error);
        ok = ok && released && closeTo(rig.memory->read(kEngineA).torque, 3800.0f) &&
             rig.tuner.exportState().empty();
        add(previouslyApplied ? "发动机后续事务：回滚失败保留原厂基准，重试不叠加"
                              : "发动机首次事务：回滚失败导出凭据，重试不把残留当原厂",
            ok, fmt("残留=%.1f 保存字节=%d 重试=%d 原厂=%.1f %s", residual, (int)saved.size(),
                    (int)retried, baseline.torque, error.c_str()));

        Rig restart;
        FakeEngineValues mixed = kVolvo;
        mixed.torque = residual;
        mixed.copyA = residualValues.copyA;
        mixed.copyB = residualValues.copyB;
        mixed.rpmLimit = residualValues.rpmLimit;
        mixed.rpmLimitNeutral = residualValues.rpmLimitNeutral;
        restart.memory->mapEngine(kEngineA, mixed);
        restart.resolver->setObject(kEngineA);
        restart.tuner.importState(saved);
        const bool restarted = restart.tuner.apply(1.25f, false, &error);
        add("发动机部分事务：导出重载后先恢复，再按真实原厂倍率写入",
            restarted && closeTo(restart.memory->read(kEngineA).torque, 4750.0f) &&
            restart.tuner.baselineOf(kEngineA, &baseline) && closeTo(baseline.torque, 3800.0f), error);

        mixed.torque = 9100.0f;
        restart.tuner.release(&error);
        restart.memory->mapEngine(kEngineA, mixed);
        restart.tuner.importState(saved);
        const auto writes = restart.memory->writes();
        const bool refused = !restart.tuner.apply(1.25f, false, &error);
        add("发动机部分事务：重载凭据不符时保留记录，不覆盖外部内容",
            refused && restart.memory->writes() == writes &&
            closeTo(restart.memory->read(kEngineA).torque, 9100.0f) &&
            restart.tuner.exportState().find(";U") != std::string::npos, error);
    }
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        rig.memory->failWriteAndRollback(kEngineA + engine_runtime::kRpmLimit,
                                        kEngineA + engine_runtime::kTorque);
        std::string error;
        const bool failed = !rig.tuner.apply(1.25f, false, &error);
        const auto saved = rig.tuner.exportState();
        rig.memory->setUsable(false);
        const bool unreadableKept = !rig.tuner.release(&error) && rig.tuner.exportState() == saved;
        rig.memory->setUsable(true);
        rig.memory->clearInjectedFailures();
        const bool released = rig.tuner.release(&error);
        add("发动机首次部分事务：关闭也会找到无 activeObject 的恢复任务",
            failed && unreadableKept && released && closeTo(rig.memory->read(kEngineA).torque, 3800.0f) &&
            rig.tuner.exportState().empty(), error);
    }
    {
        Rig rig;
        rig.memory->mapEngine(kEngineA, kVolvo);
        rig.resolver->setObject(kEngineA);
        rig.memory->failWriteAndRollback(kEngineA + engine_runtime::kTorqueCurveTail,
                                        kEngineA + engine_runtime::kRpmLimitNeutral);
        std::string error;
        const bool failed = !rig.tuner.apply(1.25f, true, &error);
        const auto residual = rig.memory->read(kEngineA);
        const auto saved = rig.tuner.exportState();
        const bool knownResidual = closeTo(residual.torque, 3800.0f) &&
            closeTo(residual.rpmLimit, 2000.0f) && closeTo(residual.rpmLimitNeutral, 2200.0f);
        rig.memory->clearInjectedFailures();
        const bool released = rig.tuner.release(&error);
        add("发动机逐字段回滚：仅空挡 RPM 残留也保留并恢复真实基准",
            failed && knownResidual && saved.find(";U") != std::string::npos && released &&
            closeTo(rig.memory->read(kEngineA).rpmLimitNeutral, 2000.0f) &&
            rig.tuner.exportState().empty(), error);
    }
    return items;
}

}  // namespace ets2
