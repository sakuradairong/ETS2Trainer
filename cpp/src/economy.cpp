// economy.cpp —— 金钱 / 经验结构化定位的校验实现（只读，不写游戏内存）
#include "economy.h"

#include "common.h"

#include <cmath>

namespace ets2 {

namespace {

bool readI64(const ProcessMemory& memory, uint64_t address, int64_t* out) {
    int64_t v = 0;
    if (!memory.read(address, &v, sizeof(v))) return false;
    if (out) *out = v;
    return true;
}

bool readI32(const ProcessMemory& memory, uint64_t address, int32_t* out) {
    int32_t v = 0;
    if (!memory.read(address, &v, sizeof(v))) return false;
    if (out) *out = v;
    return true;
}

bool readF32(const ProcessMemory& memory, uint64_t address, float* out) {
    float v = 0.0f;
    if (!memory.read(address, &v, sizeof(v))) return false;
    if (!std::isfinite(v)) return false;
    if (out) *out = v;
    return true;
}

bool isCanonical(uint64_t value) {
    return value >= 0x10000ull && value <= 0x00007FFFFFFFFFFFull;
}

// 0x31 类型（s64）在存档里都是小整数（0/1 开关或金额）
bool plausibleSmallCounter(int64_t value) {
    return value >= 0 && value <= 1000000;
}

}  // namespace

bool probeBankObject(const ProcessMemory& memory, uint64_t moneyAddress, ProbeResult* out) {
    ProbeResult result;
    result.maxScore = 7;
    if (!isCanonical(moneyAddress) || moneyAddress < economy_offsets::kBankMoneyAccount) return false;
    result.address = moneyAddress;
    result.object = moneyAddress - economy_offsets::kBankMoneyAccount;

    int64_t money = 0;
    if (!readI64(memory, moneyAddress, &money)) return false;
    result.value = money;
    std::string detail;
    // 金额本身：允许为负（欠款），但必须在合理量级内
    if (money >= -1000000000000LL && money <= 1000000000000LL) {
        result.score += 2;
        detail += "金额量级正常";
    } else {
        detail += "金额量级异常";
    }

    const uint64_t bank = result.object;
    float coinsuranceFixed = 0.0f, coinsuranceRatio = 0.0f, loanLimit = 0.0f, overdraft = 0.0f;
    if (readF32(memory, bank + economy_offsets::kBankCoinsuranceFixed, &coinsuranceFixed) &&
        coinsuranceFixed >= 0.0f && coinsuranceFixed <= 100000.0f) {
        result.score += 1;
        detail += " · 免赔额正常";
    } else {
        detail += " · 免赔额异常";
    }
    if (readF32(memory, bank + economy_offsets::kBankCoinsuranceRatio, &coinsuranceRatio) &&
        coinsuranceRatio >= 0.0f && coinsuranceRatio <= 100.0f) {
        result.score += 1;
        if (coinsuranceRatio <= 1.0f) result.score += 1;  // 0~1 更符合共保比例语义
        detail += " · 共保比例正常";
    } else {
        detail += " · 共保比例异常";
    }
    if (readF32(memory, bank + economy_offsets::kBankLoanLimit, &loanLimit) && loanLimit >= 0.0f &&
        loanLimit <= 100000000.0f) {
        result.score += 1;
        detail += " · 贷款额度正常";
    } else {
        detail += " · 贷款额度异常";
    }
    if (readF32(memory, bank + economy_offsets::kBankOverdraft, &overdraft) && overdraft >= 0.0f &&
        overdraft <= 10000000.0f) {
        result.score += 1;
        detail += " · 透支额正常";
    } else {
        detail += " · 透支额异常";
    }

    int32_t overdraftTimer = 0;
    if (readI32(memory, bank + economy_offsets::kBankOverdraftTimer, &overdraftTimer) &&
        overdraftTimer >= 0 && overdraftTimer <= 100000000) {
        detail += " · 计时正常";
    } else {
        detail += " · 计时异常";
    }

    int64_t appEnabled = 0;
    if (readI64(memory, bank + economy_offsets::kBankAppEnabled, &appEnabled) &&
        plausibleSmallCounter(appEnabled)) {
        detail += " · 银行 App 标志正常";
    } else {
        detail += " · 银行 App 标志异常";
    }

    result.detail = detail;
    if (out) *out = result;
    // 至少「金额量级 + 两个浮点字段」通过才认为值得让用户确认
    return result.score >= 4;
}

bool probeEconomyObject(const ProcessMemory& memory, uint64_t xpAddress, ProbeResult* out) {
    ProbeResult result;
    result.maxScore = 9;
    if (!isCanonical(xpAddress) ||
        xpAddress < economy_offsets::kEconomyExperiencePoints) {
        return false;
    }
    result.address = xpAddress;
    result.object = xpAddress - economy_offsets::kEconomyExperiencePoints;

    int32_t xp = 0;
    if (!readI32(memory, xpAddress, &xp)) return false;
    result.value = xp;
    const uint64_t economy = result.object;

    // ---- 硬性条件：不满足直接判定为「认错地址」，不给用户任何可疑结果 ----
    // 1) 经验地址前面必须紧挨着合理的存档版本字段（真实经济对象 @+0x77C）
    int32_t version = 0;
    if (!readI32(memory, economy + economy_offsets::kEconomySaveGameVersion, &version) ||
        version < 1 || version > 10000) {
        result.score = 0;
        result.detail = "存档版本字段不合理（疑似偏移错误）";
        if (out) *out = result;
        return false;
    }
    // 2) 经济对象必须持有合法的 bank 引用（@+0x18）
    int64_t bankRefCheck = 0;
    if (!readI64(memory, economy + economy_offsets::kEconomyBankRef, &bankRefCheck) ||
        !isCanonical(bankRefCheck)) {
        result.score = 0;
        result.detail = "bank 引用不是有效指针（疑似偏移错误）";
        if (out) *out = result;
        return false;
    }

    std::string detail;
    if (xp >= 0 && xp <= 1000000000) {
        result.score += 2;
        detail += "经验值合理";
    } else {
        detail += "经验值异常";
    }
    result.score += 1;
    detail += " · 存档版本字段合理";

    // 紧跟在经验后面的 6 个技能点（adr/long_dist/heavy/fragile/urgent/mechanical）
    int skillOk = 0;
    int64_t skillSum = 0;
    for (int i = 0; i < economy_offsets::kEconomySkillCount; ++i) {
        int32_t skill = 0;
        if (readI32(memory, economy + economy_offsets::kEconomySkills + (uint64_t)i * 4, &skill) &&
            skill >= 0 && skill <= 1000000) {
            ++skillOk;
            skillSum += skill;
        }
    }
    if (skillOk == economy_offsets::kEconomySkillCount) {
        result.score += 3;
        detail += " · 6 项技能点全部合理";
    } else if (skillOk >= economy_offsets::kEconomySkillCount - 2) {
        result.score += 1;
        detail += fmt(" · 技能点 %d/6 合理", skillOk);
    } else {
        detail += fmt(" · 技能点 %d/6 合理", skillOk);
    }
    if (skillSum > 0) {
        result.score += 1;  // 非全零更像真实存档（新档可能全零，故不强制）
        detail += " · 技能点非全零";
    }

    // bank 引用已在硬性条件里校验通过，这里只记分不重复判定
    result.score += 1;
    detail += " · 银行引用合法";

    int32_t gameTimeSecs = 0;
    if (readI32(memory, economy + economy_offsets::kEconomyGameTimeSecs, &gameTimeSecs) &&
        gameTimeSecs >= 0 && gameTimeSecs <= 2000000000) {
        result.score += 1;
        detail += " · 游戏时间合理";
    } else {
        detail += " · 游戏时间异常";
    }

    result.detail = detail;
    if (out) *out = result;
    return result.score >= 6;
}

bool economyReferencesBank(const ProcessMemory& memory, uint64_t economyObject,
                           uint64_t bankObject) {
    if (!isCanonical(economyObject) || !isCanonical(bankObject)) return false;
    int64_t ref = 0;
    if (!readI64(memory, economyObject + economy_offsets::kEconomyBankRef, &ref)) return false;
    return ref == (int64_t)bankObject;
}

uint64_t economyFromBankPointer(uint64_t bankPointerSlot) {
    if (bankPointerSlot < economy_offsets::kEconomyBankRef) return 0;
    return bankPointerSlot - economy_offsets::kEconomyBankRef;
}

// 直接写入的安全范围（与 probeBankObject 的量级校验一致）
constexpr int64_t kMoneyMin = -1000000000000LL;   // 允许负数（透支/欠款）
constexpr int64_t kMoneyMax = 1000000000000LL;
constexpr int32_t kXpMin = 0;
constexpr int32_t kXpMax = 2000000000;            // 接近 int32 上限，防溢出

bool writeBankMoneyVerified(const ProcessMemory& memory, uint64_t moneyAddress,
                            uint64_t expectedBank, int64_t value, std::string* error) {
    // 1) 写入前重新校验 bank 结构：换存档/读档后旧地址可能已被复用
    ProbeResult probe;
    if (!probeBankObject(memory, moneyAddress, &probe)) {
        if (error) *error = "bank 结构校验未通过，已拒绝写入（可能已换存档或重载，请重新定位）";
        return false;
    }
    // 2) 对象身份比对：校验出的对象基址必须与定位时记录的一致
    if (probe.object != expectedBank) {
        if (error) *error = "bank 对象身份不一致（地址疑似被复用），已拒绝写入";
        return false;
    }
    // 3) 目标金额安全范围（允许负值＝欠款）
    if (value < kMoneyMin || value > kMoneyMax) {
        if (error) *error = fmt("目标金额超出安全范围（%s ~ %s）",
                                formatInt(kMoneyMin).c_str(), formatInt(kMoneyMax).c_str());
        return false;
    }
    // 4) 写入 + 回读比对
    if (!memory.write(moneyAddress, &value, sizeof(value))) {
        if (error) *error = "写入失败（地址可能已失效，请重新定位）";
        return false;
    }
    int64_t readBack = 0;
    if (!memory.read(moneyAddress, &readBack, sizeof(readBack)) || readBack != value) {
        if (error) *error = "回读校验失败（写入未生效）";
        return false;
    }
    return true;
}

bool writeEconomyExperienceVerified(const ProcessMemory& memory, uint64_t xpAddress,
                                    uint64_t expectedEconomy, int32_t value, std::string* error) {
    // 1) 写入前重新校验经济对象结构（存档版本 / bank 引用 / 技能点布局）
    ProbeResult probe;
    if (!probeEconomyObject(memory, xpAddress, &probe)) {
        if (error) *error = "经济对象结构校验未通过，已拒绝写入（可能已换存档或重载，请重新定位）";
        return false;
    }
    // 2) 对象身份比对：校验出的对象基址必须与定位时记录的一致
    if (probe.object != expectedEconomy) {
        if (error) *error = "经济对象身份不一致（地址疑似被复用），已拒绝写入";
        return false;
    }
    // 3) 经验安全范围
    if (value < kXpMin || value > kXpMax) {
        if (error) *error = fmt("目标经验超出安全范围（0 ~ %s）", formatInt(kXpMax).c_str());
        return false;
    }
    // 4) 写入 + 回读比对
    if (!memory.write(xpAddress, &value, sizeof(value))) {
        if (error) *error = "写入失败（地址可能已失效，请重新定位）";
        return false;
    }
    int32_t readBack = 0;
    if (!memory.read(xpAddress, &readBack, sizeof(readBack)) || readBack != value) {
        if (error) *error = "回读校验失败（写入未生效）";
        return false;
    }
    return true;
}

}  // namespace ets2
