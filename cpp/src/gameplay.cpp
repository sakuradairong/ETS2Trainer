// gameplay.cpp —— 游戏选择状态与描述表（唯一来源）
#include "gameplay.h"

#include <atomic>

namespace ets2 {
namespace {

// 索引顺序必须与 GameId 一致。
const GameDescriptor kDescriptors[] = {
    {GameId::Ets2, "ets2", "Euro Truck Simulator 2", "欧卡2", L"eurotrucks2.exe",
     "Euro Truck Simulator 2", "227300", 1u, true},
    {GameId::Ats, "ats", "American Truck Simulator", "美卡", L"amtrucks.exe",
     "American Truck Simulator", "270880", 2u, true},
};

size_t indexOf(GameId id) { return id == GameId::Ats ? 1u : 0u; }

std::atomic<int>& selectionSlot() {
    // 默认欧卡2，保持历史行为。
    static std::atomic<int> value{static_cast<int>(GameId::Ets2)};
    return value;
}

std::atomic<bool>& cliLockSlot() {
    static std::atomic<bool> locked{false};
    return locked;
}

}  // namespace

const GameDescriptor& gameDescriptor(GameId id) { return kDescriptors[indexOf(id)]; }

GameId selectedGame() {
    return static_cast<GameId>(selectionSlot().load(std::memory_order_acquire));
}

void setSelectedGame(GameId id) {
    selectionSlot().store(static_cast<int>(indexOf(id)), std::memory_order_release);
}

const GameDescriptor& selectedGameDescriptor() { return gameDescriptor(selectedGame()); }

void lockSelectionFromCli() { cliLockSlot().store(true, std::memory_order_release); }

bool selectionLockedFromCli() { return cliLockSlot().load(std::memory_order_acquire); }

const wchar_t* selectedProcessName() { return selectedGameDescriptor().processName; }

bool parseGameKey(const std::string& key, GameId* out) {
    if (key == "ets2") {
        if (out) *out = GameId::Ets2;
        return true;
    }
    if (key == "ats") {
        if (out) *out = GameId::Ats;
        return true;
    }
    return false;
}

GameId gameIdFromKey(const std::string& key, GameId fallback) {
    GameId id = fallback;
    return parseGameKey(key, &id) ? id : fallback;
}

const char* gameKey(GameId id) { return gameDescriptor(id).key; }

bool supportsVerifiedStructures(GameId id) { return gameDescriptor(id).verifiedLayouts; }

std::string structuralWriteBlockReason() {
    const GameDescriptor& d = selectedGameDescriptor();
    if (d.verifiedLayouts) return std::string();
    return std::string("美卡布局尚未验证：") + d.shortName +
           " 的固定地址与结构偏移未按本版本核对，已拒绝结构化写入"
           "（现金/经验结构化定位、固定油量、车辆无损、动力调节）。"
           "手动扫描、存档编辑与控制台输入仍可使用。";
}

}  // namespace ets2
