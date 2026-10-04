// gameplay.h —— 游戏选择（欧卡2 / 美卡）与各游戏进程/文档/云存档描述
#pragma once

#include <string>

namespace ets2 {

enum class GameId { Ets2 = 0, Ats = 1 };

struct GameDescriptor {
    GameId         id;
    const char*    key;             // settings 持久化用："ets2" / "ats"
    const char*    displayName;     // 界面标题（ASCII）
    const char*    shortName;       // 选择器短名（UTF-8）
    const wchar_t* processName;     // 目标进程可执行文件名
    const char*    documentsSubdir; // "文档" 下的游戏目录
    const char*    steamAppId;      // Steam userdata 云存档 appid
    unsigned       telemetryGame;   // 共享内存 game 字段：1=ETS2, 2=ATS
    bool           verifiedLayouts; // 固定静态布局是否已按本版本核对
};

// 全部可选游戏（顺序即选择器顺序）。
inline constexpr GameId kAllGameIds[] = {GameId::Ets2, GameId::Ats};

const GameDescriptor& gameDescriptor(GameId id);

// 当前选中的游戏：进程发现、文档/云存档路径、遥测校验全部以它为准。
// selectionSlot 是原子量，可在工作线程运行时安全读取；切换写入仍要求工作线程静止。
GameId                selectedGame();
void                  setSelectedGame(GameId id);
// --game=... 明确指定后锁定选择：settings.ini 不得覆盖命令行。
void lockSelectionFromCli();
bool selectionLockedFromCli();
const GameDescriptor& selectedGameDescriptor();
const wchar_t*        selectedProcessName();

// 命令行 --game=ets2 / --game=ats。未知键返回 false（不改变选择）。
bool        parseGameKey(const std::string& key, GameId* out);
GameId      gameIdFromKey(const std::string& key, GameId fallback = GameId::Ets2);
const char* gameKey(GameId id);

// 固定静态布局（经济/发动机偏移与车辆字段）是否已验证；生产写入仍须校验实际进程版本及对象。
bool supportsVerifiedStructures(GameId id);
// 结构化写入闸门：返回空串表示允许，否则是中文拒绝原因（含“美卡布局尚未验证”提示）。
std::string structuralWriteBlockReason();

}  // namespace ets2
