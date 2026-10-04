// economy.h —— 金钱 / 经验的结构化定位（基于 ETS2 1.61.1.1 逆向出的单位字段表）
//
// 逆向依据（eurotrucks2.exe 1.61.1.1，静态分析类描述符表）：
//   类 "bank"（def /def/bank_data.sii）字段表 @RVA 0x2D7F8A0：
//        money_account       s64  +0x18
//        coinsurance_fixed   f32  +0x20
//        coinsurance_ratio   f32  +0x24
//        loan_limit          f32  +0x68
//        overdraft           f32  +0x70
//        overdraft_timer     s32  +0x74
//        app_enabled         s64  +0x58
//   经济单位字段表 @RVA 0x2D54788（148 个字段）：
//        bank                unit* +0x18   ← 指向上面的 bank 对象
//        game_time           f32   +0x1A0
//        game_time_secs      s32   +0x1A4
//        save_game_version   s32   +0x77C
//        experience_points   s32   +0x780
//        adr/long_dist/heavy/fragile/urgent/mechanical  s32 +0x784..+0x798
//
// 因此：金钱 = *(economy + 0x18) + 0x18，经验 = economy + 0x780。
// 定位策略：先按用户输入的当前金钱找到 bank 对象（结构与浮点字段校验），
// 再在全内存里找「指向该 bank 的指针」得到经济对象，经验地址随之确定。
// 本模块只做读取与校验，不做任何写入。
#pragma once

#include "memory.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ets2 {

namespace economy_offsets {

// bank 单位
constexpr uint64_t kBankDefPathField = 0x10;
constexpr uint64_t kBankMoneyAccount = 0x18;      // s64
constexpr uint64_t kBankCoinsuranceFixed = 0x20;  // f32
constexpr uint64_t kBankCoinsuranceRatio = 0x24;  // f32
constexpr uint64_t kBankAppEnabled = 0x58;        // s64
constexpr uint64_t kBankLoanLimit = 0x68;         // f32
constexpr uint64_t kBankOverdraft = 0x70;         // f32
constexpr uint64_t kBankOverdraftTimer = 0x74;    // s32

// 经济单位
constexpr uint64_t kEconomyBankRef = 0x18;          // unit* → bank
constexpr uint64_t kEconomyGameTime = 0x1A0;        // f32
constexpr uint64_t kEconomyGameTimeSecs = 0x1A4;    // s32
constexpr uint64_t kEconomySaveGameVersion = 0x77C; // s32
constexpr uint64_t kEconomyExperiencePoints = 0x780; // s32
constexpr uint64_t kEconomySkills = 0x784;          // s32[6]
constexpr int      kEconomySkillCount = 6;

}  // namespace economy_offsets

// ATS 1.61.3.1 runtime bank/economy, verified against serialized unit identifiers
// and values. These are distinct from the definition descriptor offsets above.
namespace ats_economy_offsets {
constexpr uint64_t kMoney = 0x10, kBankRef = 0x10, kXp = 0x77C, kSkills = 0x780;
}
uint64_t bankMoneyOffset();
uint64_t economyXpOffset();
uint64_t economyBankRefOffset();

// 一个候选地址的校验结果：score 越高越可信，detail 供界面显示。
struct ProbeResult {
    uint64_t address = 0;   // 被校验的字段地址（金钱地址 / 经验地址）
    uint64_t object = 0;    // 该字段所属对象（bank 对象 / 经济对象）
    int      score = 0;
    int      maxScore = 0;
    int64_t  value = 0;     // 读到的字段值
    std::string detail;     // 逐项校验说明
};

// 把「金钱地址」当作 bank 对象的 money_account 校验。
// 返回 true 表示达到了最低可信度（可交给用户确认）。
bool probeBankObject(const ProcessMemory& memory, uint64_t moneyAddress, ProbeResult* out);

// 把「经验地址」当作经济对象的 experience_points 校验。
bool probeEconomyObject(const ProcessMemory& memory, uint64_t xpAddress, ProbeResult* out);

// 经济对象 +0x18 是否指向指定 bank 对象（用于确认两个定位结果属于同一份存档状态）。
bool economyReferencesBank(const ProcessMemory& memory, uint64_t economyObject,
                           uint64_t bankObject);

// 从「指针指向 bank 对象」的候选地址反推经济对象地址（字段偏移 kEconomyBankRef）。
uint64_t economyFromBankPointer(uint64_t bankPointerSlot);

// ---------------------------------------------------------------------------
// 候选筛选（结构化定位的唯一入口）。
//
// 安全规则：
//   1) 只有「最高分候选唯一」（tied == 1）才允许认为定位成功；同分候选（tied > 1）
//      时 unique == false，调用方必须失败关闭，绝不取第一个候选继续自动写入。
//   2) 候选数量超过检查上限时（truncated == true）**不允许**返回可写的唯一结果：
//      未检查到的候选可能同分甚至更高分，此时必须由用户缩小范围或继续筛选。
//   银行对象与经济对象同规。
// ---------------------------------------------------------------------------
struct CandidatePick {
    bool     probed = false;     // 至少一个候选通过结构校验
    bool     unique = false;     // 最高分候选唯一且候选已全部检查（== 可以安全使用）
    bool     truncated = false;  // 候选数量超过 cap，未完整检查（unique 强制为 false）
    size_t   examined = 0;       // 实际检查的候选槽位数
    size_t   total = 0;          // 扫描到的候选槽位总数
    int      tied = 0;           // 最高分候选数量（含首个；>1 表示歧义）
    int      probedCount = 0;    // 通过结构校验的（去重后）候选对象数
    uint64_t fieldAddress = 0;   // 被选中的字段地址（金钱地址 / 经验地址）
    uint64_t object = 0;         // 字段所属对象（bank 对象 / 经济对象）
    int64_t  value = 0;          // 读到的字段值
    int      score = 0;          // 最高分
};

// 在金额候选地址中挑选唯一可信的 bank 对象（对每个候选做完整结构校验，
// 按「对象基址」去重后比较分数；同分歧义时 unique == false）。
CandidatePick pickBestBankCandidate(const ProcessMemory& memory,
                                    const std::vector<uint64_t>& moneyAddresses, size_t cap);

// 由「内容为 bank 指针的槽位地址」反推经济对象并校验。
// requiredBank 非零时：槽位内容必须等于 requiredBank，且经济对象 +0x18 的
// bank 引用必须一致，否则该候选直接排除（不参与打分，避免假歧义/假命中）。
CandidatePick pickBestEconomyFromBankRefs(const ProcessMemory& memory,
                                          const std::vector<uint64_t>& bankPointerSlots,
                                          uint64_t requiredBank, size_t cap);

// 由「经验字段地址」（扫描命中的地址本身就是 economy + 0x780，不得再加减偏移）
// 挑选唯一可信的经济对象。requiredBank 非零时必须验证经济对象确实引用该银行对象。
CandidatePick pickBestEconomyFromXpFields(const ProcessMemory& memory,
                                          const std::vector<uint64_t>& xpFieldAddresses,
                                          uint64_t requiredBank, size_t cap);

// ---------------------------------------------------------------------------
// 直接写入（免锁定）。
//
// 原理：bank.money_account 与 economy.experience_points 是游戏自己的权威存储——
// UI 显示、扣款/进账、经验结算与自动存档都直接读写这两个字段，游戏不会从别处
// 把它们「改回去」。因此一次性写入即可持久，不需要后台锁定线程。
// 锁定只保留为可选保险（防止游戏内真实收支改变数值），见 UI。
//
// 写入流程与发动机调节同一标准：写入前重新做结构校验 + 对象身份比对（对象基址必须与
// 定位时记录的一致，防止地址被复用后写进无关内存）+ 数值安全范围检查，写入后回读比对；
// 任何一步失败都不返回成功。
// ---------------------------------------------------------------------------
bool writeBankMoneyVerified(const ProcessMemory& memory, uint64_t moneyAddress,
                            uint64_t expectedBank, int64_t value, std::string* error);
bool writeEconomyExperienceVerified(const ProcessMemory& memory, uint64_t xpAddress,
                                    uint64_t expectedEconomy, int32_t value, std::string* error);

}  // namespace ets2
