// enginetest.cpp —— EngineTuner 的离线状态机与并发测试
//
// 这些测试不接触真实游戏：EngineMemory 由内存镜像实现，定位器由脚本实现。
// 覆盖：倍率不叠加、换发动机对象互相恢复、RPM 开关立即恢复、超大扭矩、
//       写入失败回滚、跨进程重启叠加保护、对象失效/复用、联机闸门、并发。
#include "enginetest.h"

#include "engine.h"

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

    return items;
}

}  // namespace ets2
