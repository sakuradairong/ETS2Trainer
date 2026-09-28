// gameio.h —— 进程/模块枚举、联机检测、管理员权限、窗口查找、控制台按键注入
#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace ets2 {

struct ProcessInfo {
    DWORD        pid = 0;
    DWORD        parent = 0;
    std::wstring exe;
};

std::vector<ProcessInfo> listProcesses();
bool                     findProcess(const std::wstring& exeName, DWORD* pidOut,
                                     std::wstring* exeOut);

struct ModuleInfo {
    std::wstring name;
    uint64_t     base = 0;
    uint64_t     size = 0;
};
std::vector<ModuleInfo> listModules(DWORD pid);
bool                    mainModule(DWORD pid, ModuleInfo* out);

//: 检测联机(TruckersMP)特征，返回命中的名字。
//: snapshotOk（可选）为 false 表示模块快照失败 —— 空命中不可信，调用方应按「无法确认」处理。
std::vector<std::wstring> detectMultiplayer(DWORD pid, bool* snapshotOk = nullptr);

//: 联机状态。Convoy 是游戏内置的官方联机，没有额外进程/模块，只能靠游戏日志判定。
enum class MultiplayerKind {
    None,        // 已确认单机
    TruckersMp,  // 检测到 TruckersMP
    Convoy,      // 官方 Convoy 会话进行中
    Unknown,     // 无法确认（例如读不到 game.log.txt）
};

//: 控制是否检测官方联运（Convoy）
void setConvoyCheckEnabled(bool enabled);
bool isConvoyCheckEnabled();

struct MultiplayerStatus {
    MultiplayerKind kind = MultiplayerKind::Unknown;
    std::string     detail;  // 判定依据（UTF-8）

    // fail-closed：TruckersMP 永远拦截；开启联运检测后，Convoy 与 Unknown（读不到日志/
    // 快照失败等一切"无法确认"状态）同样拦截。只有明确判定单机才放行写入。
    bool blocksWrite() const {
        // Unknown 在 Convoy 检测关闭时也可能表示 TruckersMP 模块枚举失败，必须拒绝。
        if (kind == MultiplayerKind::TruckersMp || kind == MultiplayerKind::Unknown) return true;
        return isConvoyCheckEnabled() && kind == MultiplayerKind::Convoy;
    }
};

//: 综合联机检测：TruckersMP 模块/进程 + 官方 Convoy（game.log.txt 的 [MP] 会话记录，受 isConvoyCheckEnabled 控制）。
//: 结果缓存 5 秒，供后台保持线程安全地高频调用；forceRefresh 用于用户操作前复核。
MultiplayerStatus checkMultiplayer(DWORD pid, bool forceRefresh = false);

// ---------------- 联运（官方 Convoy）会话信息 ----------------
struct ConvoyPlayer {
    std::string name;
    int         clientId = 0;
    bool        you = false;  // 日志里带 [you] 标记的本地玩家
};

struct ConvoySession {
    bool                       active = false;   // 会话进行中
    bool                       logAvailable = false;
    std::string                startedAt;        // 最近一次会话开始的时间戳（游戏内时钟）
    std::vector<ConvoyPlayer>  players;
    std::string                note;
};

//: 解析 game.log.txt 文本里的 [MP] 会话记录（纯函数，便于离线自检）
bool parseConvoySession(const std::string& logText, ConvoySession* out);

//: 读取 game.log.txt 尾部并解析当前 Convoy 会话
ConvoySession readConvoySession();

bool isAdmin();
bool enableDebugPrivilege();

// ---------------- 窗口 / 按键 ----------------
uintptr_t findGameWindow(DWORD pid = 0);
bool      activateWindow(uintptr_t hwnd);
int       consoleKeyFromControls(const std::wstring& controlsPath);

struct SendResult {
    bool        ok = false;
    int         typed = 0;
    std::string message;
};
SendResult sendConsoleCommand(const std::string& command, int consoleVk, DWORD pid,
                              bool closeAfter = true);

//: 虚拟键码预设（UI 下拉框用）
struct ConsoleKeyOption {
    const char* label;
    int         vk;
};
const std::vector<ConsoleKeyOption>& consoleKeyOptions();

// ---------------- 飞行模式（自由相机）----------------
//: 自由相机的按键绑定（来自 controls.sii 的 mix cam* 行）
struct CameraBindings {
    int  toggle = 0;   // mix camdbg：开/关自由相机
    int  forward = 0;  // mix camfwd
    int  back = 0;     // mix camback
    int  left = 0;     // mix camleft
    int  right = 0;    // mix camright
    int  up = 0;       // mix camup
    int  down = 0;     // mix camdown
    bool found = false;
};

//: 把 controls.sii 里的按键名（keyboard.key0 / keyboard.num9 / keyboard.grave ...）转成虚拟键码
int keyNameToVirtualKey(const std::string& name);

//: 从 controls.sii 文本解析自由相机绑定（纯函数，便于离线自检）
bool parseCameraBindings(const std::string& text, CameraBindings* out);

//: 读取 controls.sii 并解析（找不到文件时返回 false）
bool cameraKeyBindings(const std::wstring& controlsPath, CameraBindings* out);

//: 把游戏窗口切到前台并敲一个键（例如切换自由相机）
SendResult sendKeyTap(int vk, DWORD pid);

}  // namespace ets2
