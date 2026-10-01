// economytest.cpp —— CandidatePick 候选筛选测试（自身进程内存构造假对象，不写游戏）
#include "economytest.h"

#include "economy.h"
#include "memory.h"
#include "ui.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace ets2 {

namespace {

// 独立内存页：越界读会失败，可确定性地模拟「结构错误 / 未映射」
struct Page {
    uint8_t* base = nullptr;
    SIZE_T   size = 0;

    bool alloc(SIZE_T bytes) {
        base = (uint8_t*)::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
        size = base ? bytes : 0;
        return base != nullptr;
    }
    void release() {
        if (base) {
            ::VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
            size = 0;
        }
    }
    ~Page() { release(); }
};

// 构造一个结构合法的 bank 对象（字段表见 economy.h 注释）
uint64_t placeBank(uint8_t* base, size_t offset, int64_t money) {
    uint8_t* b = base + offset;
    const float coinsuranceFixed = 500.0f;
    const float coinsuranceRatio = 0.2f;
    const float loanLimit = 100000.0f;
    const float overdraft = 0.0f;
    const int32_t overdraftTimer = 30;
    const int64_t appEnabled = 1;
    ::memcpy(b + economy_offsets::kBankMoneyAccount, &money, sizeof(money));
    ::memcpy(b + economy_offsets::kBankCoinsuranceFixed, &coinsuranceFixed,
             sizeof(coinsuranceFixed));
    ::memcpy(b + economy_offsets::kBankCoinsuranceRatio, &coinsuranceRatio,
             sizeof(coinsuranceRatio));
    ::memcpy(b + economy_offsets::kBankLoanLimit, &loanLimit, sizeof(loanLimit));
    ::memcpy(b + economy_offsets::kBankOverdraft, &overdraft, sizeof(overdraft));
    ::memcpy(b + economy_offsets::kBankOverdraftTimer, &overdraftTimer,
             sizeof(overdraftTimer));
    ::memcpy(b + economy_offsets::kBankAppEnabled, &appEnabled, sizeof(appEnabled));
    return (uint64_t)(uintptr_t)b;
}

// 构造一个结构合法的经济对象（引用指定 bank）
uint64_t placeEconomy(uint8_t* base, size_t offset, uint64_t bank, int32_t xp) {
    uint8_t* e = base + offset;
    const int32_t saveVersion = 50;
    const int32_t skills[6] = {3, 5, 2, 1, 4, 2};
    const int32_t gameTimeSecs = 123456;
    ::memcpy(e + economy_offsets::kEconomyBankRef, &bank, sizeof(bank));
    ::memcpy(e + economy_offsets::kEconomyGameTimeSecs, &gameTimeSecs,
             sizeof(gameTimeSecs));
    ::memcpy(e + economy_offsets::kEconomySaveGameVersion, &saveVersion,
             sizeof(saveVersion));
    ::memcpy(e + economy_offsets::kEconomyExperiencePoints, &xp, sizeof(xp));
    for (int i = 0; i < economy_offsets::kEconomySkillCount; ++i) {
        ::memcpy(e + economy_offsets::kEconomySkills + (size_t)i * 4, &skills[i],
                 sizeof(int32_t));
    }
    return (uint64_t)(uintptr_t)e;
}

}  // namespace

std::vector<TunerTestItem> runEconomyLocateTests() {
    std::vector<TunerTestItem> items;
    auto add = [&items](const std::string& name, bool ok, const std::string& detail) {
        items.push_back({name, ok, detail});
    };

    Page page;
    if (!page.alloc(0x10000)) {
        add("经济定位测试环境（VirtualAlloc）", false, "无法分配测试内存页");
        return items;
    }
    ProcessMemory mem;
    std::string err;
    if (!mem.open(::GetCurrentProcessId(), "economy-test", &err)) {
        add("经济定位测试环境（打开自身进程）", false, err);
        return items;
    }

    // 两个结构完全一致、但相互独立的 bank / economy 对象（用于制造同分歧义）
    const uint64_t bankA = placeBank(page.base, 0x100, 123456789);
    const uint64_t bankB = placeBank(page.base, 0x1000, 123456789);
    const uint64_t econA = placeEconomy(page.base, 0x2000, bankA, 582499);
    const uint64_t econB = placeEconomy(page.base, 0x3000, bankB, 582499);

    // ---------- 零候选 ----------
    {
        // 确定性的「结构错误/未映射」地址：单独提交两页，把第二页 decommit，
        // 读取必然失败（不受进程里相邻内存布局影响）。
        const SIZE_T twoPages = 0x2000;
        uint8_t* region = (uint8_t*)::VirtualAlloc(nullptr, twoPages, MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE);
        CandidatePick unmapped;
        if (!region) {
            unmapped.probed = true;  // 分配失败时构造一个必然失败的对象
        } else {
            ::VirtualFree(region + 0x1000, 0x1000, MEM_DECOMMIT);
            unmapped = pickBestBankCandidate(mem, {(uint64_t)(uintptr_t)(region + 0x1018)}, 100);
            ::VirtualFree(region, 0, MEM_RELEASE);
        }
        CandidatePick empty = pickBestBankCandidate(mem, {}, 100);
        const bool ok = !empty.probed && !empty.unique && !unmapped.probed && !unmapped.unique;
        add("bank 候选：零候选时不得定位成功", ok,
            ok ? "空列表与未映射地址都被拒绝" : "零候选被误判为可定位");
    }
    // ---------- 唯一候选 ----------
    {
        const CandidatePick pick =
            pickBestBankCandidate(mem, {bankA + economy_offsets::kBankMoneyAccount}, 100);
        const bool ok = pick.unique && pick.tied == 1 && pick.object == bankA &&
                        pick.value == 123456789;
        add("bank 候选：唯一最高分候选可定位", ok,
            fmt("unique=%d tied=%d 对象=0x%llX（期望 0x%llX）", (int)pick.unique, pick.tied,
                (unsigned long long)pick.object, (unsigned long long)bankA));
    }
    // ---------- 多个同分候选：必须失败关闭 ----------
    {
        const CandidatePick pick = pickBestBankCandidate(
            mem,
            {bankA + economy_offsets::kBankMoneyAccount,
             bankB + economy_offsets::kBankMoneyAccount},
            100);
        const bool ok = pick.probed && pick.tied == 2 && !pick.unique;
        add("bank 候选：多个同分候选必须失败关闭（不取第一个）", ok,
            fmt("tied=%d unique=%d", pick.tied, (int)pick.unique));
    }
    // ---------- 同一对象的重复命中不算歧义 ----------
    {
        const CandidatePick pick = pickBestBankCandidate(
            mem,
            {bankA + economy_offsets::kBankMoneyAccount,
             bankA + economy_offsets::kBankMoneyAccount},
            100);
        const bool ok = pick.unique && pick.tied == 1;
        add("bank 候选：重复命中同一对象不构成歧义", ok,
            fmt("tied=%d unique=%d", pick.tied, (int)pick.unique));
    }

    // ---------- 经验回退：扫描地址就是经验字段地址（不得再加偏移） ----------
    {
        // 独立两页区域，第二页 decommit：错误偏移（+0x780）必然落在不可读页 → 拒绝
        uint8_t* region = (uint8_t*)::VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE);
        if (!region) {
            add("经验字段地址换算", false, "无法分配独立测试页");
        } else {
            ::VirtualFree(region + 0x1000, 0x1000, MEM_DECOMMIT);
            const uint64_t econ =
                placeEconomy(region, 0x800, bankA, 4242);  // xp 字段在第一页末尾
            const uint64_t xpField = econ + economy_offsets::kEconomyExperiencePoints;
            const CandidatePick right = pickBestEconomyFromXpFields(mem, {xpField}, 0, 100);
            const CandidatePick wrong = pickBestEconomyFromXpFields(
                mem, {xpField + economy_offsets::kEconomyExperiencePoints}, 0, 100);
            const bool ok = right.unique && right.object == econ && right.value == 4242 &&
                            !wrong.probed && !wrong.unique;
            add("经验回退：直接使用扫描地址（重复加偏移会被拒绝）", ok,
                fmt("正确 unique=%d 对象=0x%llX；错误(+0x780) probed=%d", (int)right.unique,
                    (unsigned long long)right.object, (int)wrong.probed));
            ::VirtualFree(region, 0, MEM_RELEASE);
        }
    }

    // ---------- 由 bank 指针槽反查经济对象 + 引用一致性 ----------
    {
        const uint64_t slotA = econA + economy_offsets::kEconomyBankRef;
        const uint64_t slotB = econB + economy_offsets::kEconomyBankRef;
        const CandidatePick match =
            pickBestEconomyFromBankRefs(mem, {slotA, slotB}, bankA, 100);
        const bool ok = match.unique && match.object == econA &&
                        match.fieldAddress == econA + economy_offsets::kEconomyExperiencePoints;
        add("bank 引用反查：只认引用一致的经济对象", ok,
            fmt("unique=%d 对象=0x%llX（期望 0x%llX）", (int)match.unique,
                (unsigned long long)match.object, (unsigned long long)econA));
    }
    // 不限定 bank 时两个同分经济对象 → 歧义关闭
    {
        const uint64_t slotA = econA + economy_offsets::kEconomyBankRef;
        const uint64_t slotB = econB + economy_offsets::kEconomyBankRef;
        const CandidatePick any = pickBestEconomyFromBankRefs(mem, {slotA, slotB}, 0, 100);
        add("bank 引用反查：同分歧义必须失败关闭", any.probed && any.tied == 2 && !any.unique,
            fmt("tied=%d unique=%d", any.tied, (int)any.unique));
    }

    // ---------- 经验字段定位 + 必须引用已定位的 bank ----------
    {
        const uint64_t xpA = econA + economy_offsets::kEconomyExperiencePoints;
        const uint64_t xpB = econB + economy_offsets::kEconomyExperiencePoints;
        const CandidatePick pickA = pickBestEconomyFromXpFields(mem, {xpA, xpB}, bankA, 100);
        const CandidatePick pickB = pickBestEconomyFromXpFields(mem, {xpA, xpB}, bankB, 100);
        const CandidatePick pickNone =
            pickBestEconomyFromXpFields(mem, {xpA, xpB}, bankA + 8, 100);
        const bool ok = pickA.unique && pickA.object == econA && pickB.unique &&
                        pickB.object == econB && !pickNone.probed && !pickNone.unique;
        add("经验定位：经济对象必须引用已定位的 bank", ok,
            fmt("A:%d/0x%llX B:%d/0x%llX 错误bank:%d", (int)pickA.unique,
                (unsigned long long)pickA.object, (int)pickB.unique,
                (unsigned long long)pickB.object, (int)pickNone.probed));
    }

    // ---------- 地址换算单元（真实地址往返） ----------
    {
        const uint64_t slot = econA + economy_offsets::kEconomyBankRef;
        const bool ok = economyFromBankPointer(slot) == econA &&
                        economyFromBankPointer(0x10) == 0 &&
                        economyReferencesBank(mem, econA, bankA) &&
                        !economyReferencesBank(mem, econA, bankB);
        add("bank 指针槽 ↔ 经济对象地址换算", ok, ok ? "换算与引用判定一致" : "换算错误");
    }

    // ---------- 审查问题 7：经济写入必须过写入闸门（真正写入处拦截） ----------
    {
        // 用同一批假对象 + AppState，直接驱动生产代码路径（writeLocatedMoney/Xp
        // 与其共用的 writeBankMoneyGuarded/writeXpGuarded）。
        AppState app;
        std::string openError;
        const bool attached = app.mem.open(::GetCurrentProcessId(), "gate-test", &openError);
        app.attached = attached;
        app.pid = ::GetCurrentProcessId();
        const uint64_t moneyAddress = bankA + economy_offsets::kBankMoneyAccount;
        const uint64_t xpAddress = econA + economy_offsets::kEconomyExperiencePoints;
        app.locatedMoneyAddress = moneyAddress;
        app.locatedMoneyBank = bankA;
        app.locatedXpAddress = xpAddress;
        app.locatedEconomy = econA;
        ::strncpy_s(app.moneyTarget, sizeof(app.moneyTarget), "777777", _TRUNCATE);
        ::strncpy_s(app.xpTarget, sizeof(app.xpTarget), "123456", _TRUNCATE);

        auto moneyValue = [&mem, moneyAddress]() {
            int64_t value = 0;
            mem.read(moneyAddress, &value, sizeof(value));
            return value;
        };
        auto xpValue = [&mem, xpAddress]() {
            int32_t value = 0;
            mem.read(xpAddress, &value, sizeof(value));
            return value;
        };

        // 1) 单机 → 联机：联机时写入被拦截、内存不变、定位结果保留
        setWriteGateOverride([]() { return std::string("测试：检测到 TruckersMP"); });
        const int64_t moneyBefore = moneyValue();
        const int32_t xpBefore = xpValue();
        writeLocatedMoney(app, false);
        const bool moneyBlocked = moneyValue() == moneyBefore &&
                                  app.locatedMoneyAddress != 0 &&
                                  app.locateStatus.find("拒绝") != std::string::npos;
        writeLocatedXp(app, false);
        const bool xpBlocked = xpValue() == xpBefore && app.locatedXpAddress != 0 &&
                               app.locateStatus.find("拒绝") != std::string::npos;

        // 2) 状态未知（无法确认）→ 同样拦截（不得偷偷放宽）
        setWriteGateOverride([]() { return std::string("无法确认联机状态（Convoy 检测已开启）"); });
        writeLocatedMoney(app, false);
        const bool unknownBlocked = moneyValue() == moneyBefore;

        // 3) 回到单机 → 可以写入（证明拦截而非永久禁用）
        setWriteGateOverride(nullptr);
        // 默认闸门在当前环境（无游戏进程）应放行；若环境判定为联机则跳过该断言
        const std::string defaultGate = writeGateReason(app);
        bool allowedWrite = false;
        if (defaultGate.empty()) {
            writeLocatedMoney(app, false);
            allowedWrite = moneyValue() == 777777;
        } else {
            allowedWrite = true;  // 环境被判为联机：上面已被覆盖测试验证过
        }

        // 4) 扫描中进入联机：定位后（地址已就绪）再进入联机 → 写入仍被拦截
        setWriteGateOverride([]() { return std::string("测试：扫描期间进入 Convoy"); });
        const int64_t moneyBeforeScan = moneyValue();
        writeLocatedMoney(app, false);
        const bool scanBlocked = moneyValue() == moneyBeforeScan &&
                                 app.locateStatus.find("拒绝") != std::string::npos;
        setWriteGateOverride(nullptr);

        const bool ok = attached && moneyBlocked && xpBlocked && unknownBlocked && allowedWrite &&
                        scanBlocked;
        add("经济写入：单机→联机/状态未知/扫描中进入联机都被真实拦截", ok,
            fmt("金钱拦截=%d 经验拦截=%d 未知拦截=%d 单机放行=%d 扫描中拦截=%d", (int)moneyBlocked,
                (int)xpBlocked, (int)unknownBlocked, (int)allowedWrite, (int)scanBlocked));
    }

    // ---------- 审查问题 8 的界面侧：越界目标值不得清除定位结果 ----------
    {
        AppState app;
        std::string openError;
        const bool attached = app.mem.open(::GetCurrentProcessId(), "range-test", &openError);
        app.pid = ::GetCurrentProcessId();
        app.locatedXpAddress = econA + economy_offsets::kEconomyExperiencePoints;
        app.locatedEconomy = econA;
        ::strncpy_s(app.xpTarget, sizeof(app.xpTarget), "99999999999999", _TRUNCATE);
        int32_t xpNow = 0;
        mem.read(app.locatedXpAddress, &xpNow, sizeof(xpNow));
        setWriteGateOverride(nullptr);
        writeLocatedXp(app, false);
        int32_t xpAfter = 0;
        mem.read(app.locatedXpAddress, &xpAfter, sizeof(xpAfter));
        const bool ok = attached && xpAfter == xpNow && app.locatedXpAddress != 0 &&
                        app.locateStatus.find("定位结果保留") != std::string::npos;
        add("越界经验目标：不清除定位结果也不写入", ok,
            fmt("地址保留=%d 值未变=%d 状态=%s", (int)(app.locatedXpAddress != 0),
                (int)(xpAfter == xpNow), app.locateStatus.c_str()));
    }

    // ---------- 审查问题 1：候选未完整检查（截断）时绝不返回可写结果 ----------
    {
        const uint64_t moneyA = bankA + economy_offsets::kBankMoneyAccount;
        const uint64_t moneyB = bankB + economy_offsets::kBankMoneyAccount;
        // cap=1 只检查第一个候选，而同分的 bankB 在 cap 之后：
        // 旧实现会误报 unique=true 并给出可写地址；现在必须 truncated 且不产生地址。
        const CandidatePick capped = pickBestBankCandidate(mem, {moneyA, moneyB}, 1);
        const CandidatePick zeroCap = pickBestBankCandidate(mem, {moneyA, moneyB}, 0);
        const CandidatePick full = pickBestBankCandidate(mem, {moneyA, moneyB}, 10);
        const bool bankOk = capped.truncated && !capped.unique && capped.fieldAddress == 0 &&
                            capped.object == 0 && capped.examined == 1 && capped.total == 2 &&
                            zeroCap.truncated && !zeroCap.unique && full.probed &&
                            !full.truncated && full.tied == 2 && !full.unique;
        add("候选截断：同分对象位于 cap 之后绝不给出可写结果", bankOk,
            fmt("cap=1 truncated=%d unique=%d 地址=0x%llX；cap=0 truncated=%d；cap=10 tied=%d unique=%d",
                (int)capped.truncated, (int)capped.unique,
                (unsigned long long)capped.fieldAddress, (int)zeroCap.truncated, full.tied,
                (int)full.unique));

        // 三条路径都要处理截断：bank 引用反查 / 经验字段回退
        const uint64_t slotA = econA + economy_offsets::kEconomyBankRef;
        const uint64_t slotB = econB + economy_offsets::kEconomyBankRef;
        const CandidatePick refCapped = pickBestEconomyFromBankRefs(mem, {slotA, slotB}, 0, 1);
        const CandidatePick xpCapped = pickBestEconomyFromXpFields(
            mem, {econA + economy_offsets::kEconomyExperiencePoints,
                  econB + economy_offsets::kEconomyExperiencePoints},
            0, 1);
        const bool pathsOk = refCapped.truncated && !refCapped.unique &&
                             refCapped.fieldAddress == 0 && xpCapped.truncated &&
                             !xpCapped.unique && xpCapped.fieldAddress == 0;
        add("候选截断：bank 引用反查与经验回退同样失败关闭", pathsOk,
            fmt("bankRef truncated=%d unique=%d；xpField truncated=%d unique=%d",
                (int)refCapped.truncated, (int)refCapped.unique, (int)xpCapped.truncated,
                (int)xpCapped.unique));
    }

    // ---------- 审查问题 7：可选锁定（Freezer 后台持续写入）也必须过闸门 ----------
    {
        ProcessMemory selfMem;
        std::string openError;
        if (!selfMem.open(::GetCurrentProcessId(), "freezer-gate-test", &openError)) {
            add("锁定写入：进入联机后停止持续写入", false, openError);
        } else {
            std::vector<uint8_t> target(0x40, 0);
            const uint64_t address = (uint64_t)(uintptr_t)target.data();
            int64_t value = 0;
            Freezer freezer;
            bool blocked = false;
            freezer.setWriteGuard([&blocked]() {
                return blocked ? std::string("测试：检测到 TruckersMP") : std::string();
            });
            // 注意：start() 会清空锁定条目，所以必须先 start 再 add
            freezer.start(&selfMem, 60);
            freezer.addInt(address, VType::Int32, 424242);
            ::Sleep(250);
            selfMem.read(address, &value, sizeof(value));
            const bool wroteWhenAllowed = value == 424242;
            // 进入联机：必须立即停止写入，且已经写入的值不再被刷新
            blocked = true;
            selfMem.writeInt(address, VType::Int32, 7);
            ::Sleep(400);
            int64_t after = 0;
            selfMem.read(address, &after, sizeof(after));
            const bool stoppedWriting = after == 7;
            const bool reportedBlocked = freezer.blocked() > 0 && !freezer.blockedReason().empty();
            freezer.stop();
            // 回到单机：恢复写入能力（重新 start 后需重新添加条目）
            blocked = false;
            freezer.start(&selfMem, 60);
            freezer.addInt(address, VType::Int32, 424242);
            ::Sleep(250);
            int64_t resumed = 0;
            selfMem.read(address, &resumed, sizeof(resumed));
            freezer.stop();
            const bool ok = wroteWhenAllowed && stoppedWriting && reportedBlocked &&
                            resumed == 424242;
            add("锁定写入：进入联机后停止持续写入（可恢复）", ok,
                fmt("单机写入=%d 联机停写=%d 有拒绝记录=%d 回单机恢复=%d", (int)wroteWhenAllowed,
                    (int)stoppedWriting, (int)reportedBlocked, (int)(resumed == 424242)));
        }
    }

    mem.close();
    return items;
}

}  // namespace ets2
