// gameio.cpp —— 进程/窗口/按键实现
#include "gameio.h"

#include "saves.h"
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace ets2 {

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
    return s;
}

}  // namespace

std::vector<ProcessInfo> listProcesses() {
    std::vector<ProcessInfo> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snap, &entry)) {
        do {
            ProcessInfo info;
            info.pid = entry.th32ProcessID;
            info.parent = entry.th32ParentProcessID;
            info.exe = entry.szExeFile;
            out.push_back(std::move(info));
        } while (::Process32NextW(snap, &entry));
    }
    ::CloseHandle(snap);
    return out;
}

bool findProcess(const std::wstring& exeName, DWORD* pidOut, std::wstring* exeOut) {
    std::wstring wanted = lower(exeName);
    if (wanted.size() < 4 || wanted.compare(wanted.size() - 4, 4, L".exe") != 0) {
        wanted += L".exe";
    }
    for (const auto& p : listProcesses()) {
        if (lower(p.exe) == wanted) {
            if (pidOut) *pidOut = p.pid;
            if (exeOut) *exeOut = p.exe;
            return true;
        }
    }
    return false;
}

std::vector<ModuleInfo> listModules(DWORD pid) {
    std::vector<ModuleInfo> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Module32FirstW(snap, &entry)) {
        do {
            ModuleInfo info;
            info.name = entry.szModule;
            info.base = (uint64_t)(uintptr_t)entry.modBaseAddr;
            info.size = entry.modBaseSize;
            out.push_back(std::move(info));
        } while (::Module32NextW(snap, &entry));
    }
    ::CloseHandle(snap);
    return out;
}

bool mainModule(DWORD pid, ModuleInfo* out) {
    std::vector<ModuleInfo> mods = listModules(pid);
    if (mods.empty()) return false;
    for (const auto& m : mods) {
        std::wstring low = lower(m.name);
        if (low.size() > 4 && low.compare(low.size() - 4, 4, L".exe") == 0) {
            if (out) *out = m;
            return true;
        }
    }
    if (out) *out = mods.front();
    return true;
}

std::vector<std::wstring> detectMultiplayer(DWORD pid, bool* snapshotOk) {
    if (snapshotOk) *snapshotOk = true;
    static const wchar_t* kMarkers[] = {L"truckersmp", L"ets2mp",  L"atsmp",
                                        L"core_ets2mp", L"core_atsmp", L"eurotrucks2mp"};
    std::vector<std::wstring> hits;
    std::vector<ModuleInfo> mods = listModules(pid);
    // 活着的进程不可能枚举不到模块：空结果意味着快照失败（权限/竞态），
    // 必须让调用方按「无法确认」处理，而不是当成"没有 TruckersMP"。
    if (mods.empty() && snapshotOk) *snapshotOk = false;
    for (const auto& m : mods) {
        std::wstring low = lower(m.name);
        for (const auto* marker : kMarkers) {
            if (low.find(marker) != std::wstring::npos) {
                hits.push_back(m.name);
                break;
            }
        }
    }
    for (const auto& p : listProcesses()) {
        std::wstring low = lower(p.exe);
        if (low.find(L"truckersmp") != std::wstring::npos ||
            low.find(L"ets2mp") != std::wstring::npos ||
            low.find(L"atsmp") != std::wstring::npos) {
            hits.push_back(p.exe);
        }
    }
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    return hits;
}

// ======================= 联机状态（TruckersMP + 官方 Convoy）=======================
namespace {

// 读取文件末尾最多 maxBytes 字节（有界读取，绝不整文件读入）。
// truncated 为真表示文件比窗口大、返回内容是从中间切开的（首行可能不完整）。
bool readFileTail(const std::wstring& path, size_t maxBytes, std::string* out,
                  bool* truncated = nullptr) {
    if (truncated) *truncated = false;
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) return false;
    const std::streamoff begin = size > (std::streamoff)maxBytes ? size - (std::streamoff)maxBytes : 0;
    if (truncated) *truncated = (begin > 0);
    in.seekg(begin, std::ios::beg);
    std::string data((size_t)(size - begin), '\0');
    in.read(data.data(), (std::streamsize)data.size());
    data.resize((size_t)in.gcount());
    *out = std::move(data);
    return true;
}

enum class LogSession { None, Running, Unknown };

// 解析 game.log.txt 里最后一次 [MP] 会话事件。
// 与 parseConvoySession 共用同一套关键词/状态机，保证「写入闸门」和「联运面板」
// 的判定永远一致（修复两套关键词不一致导致的漏判）。
// 日志每次启动游戏都会重写，因此「整个日志里没有任何 [MP] 行」可判定为本次运行未联机；
// 但窗口被截断（>512KB）时看到的"没有会话"不能证明什么，返回 Unknown。
LogSession lastConvoySession(std::string* evidence) {
    const std::wstring path = documentsDir() + L"\\game.log.txt";
    std::string tail;
    bool truncated = false;
    if (!readFileTail(path, 512 * 1024, &tail, &truncated)) return LogSession::Unknown;
    if (truncated && !tail.empty()) {
        // 丢弃可能被拦腰截断的首行，避免半行内容误触发关键词
        const size_t nl = tail.find('\n');
        if (nl != std::string::npos) {
            tail.erase(0, nl + 1);
        } else {
            tail.clear();
        }
    }
    ConvoySession session;
    parseConvoySession(tail, &session);
    if (session.active) {
        if (evidence) *evidence = session.startedAt.empty() ? "[MP] 会话记录" : session.startedAt;
        return LogSession::Running;
    }
    if (truncated) return LogSession::Unknown;  // 窗口外可能藏着会话开始行，不能断言 None
    return LogSession::None;
}

}  // namespace

static std::atomic<bool> g_convoyCheckEnabled{false};

void setConvoyCheckEnabled(bool enabled) {
    g_convoyCheckEnabled.store(enabled);
}

bool isConvoyCheckEnabled() {
    return g_convoyCheckEnabled.load();
}

MultiplayerStatus checkMultiplayer(DWORD pid, bool forceRefresh) {
    static std::mutex cacheMutex;
    static MultiplayerStatus cached;
    static DWORD cachedPid = 0;
    static uint64_t cachedAt = 0;
    static bool cachedConvoyEnabled = false;

    std::lock_guard<std::mutex> lock(cacheMutex);
    const bool convoyEnabled = isConvoyCheckEnabled();
    const uint64_t now = ::GetTickCount64();
    if (!forceRefresh && cachedPid == pid && cachedAt != 0 &&
        cachedConvoyEnabled == convoyEnabled && now - cachedAt < 5000) {
        return cached;
    }

    MultiplayerStatus status;
    bool modulesOk = true;
    std::vector<std::wstring> hits = detectMultiplayer(pid, &modulesOk);
    if (!hits.empty()) {
        status.kind = MultiplayerKind::TruckersMp;
        status.detail = "检测到 TruckersMP 特征：" + W2U(hits.front());
    } else if (!modulesOk) {
        // 无法枚举模块时也无法排除 TruckersMP，不能因为关闭了 Convoy 检测而放行。
        status.kind = MultiplayerKind::Unknown;
        status.detail = "无法枚举游戏进程模块，无法排除 TruckersMP（拒绝写入）";
    } else if (!convoyEnabled) {
        status.kind = MultiplayerKind::None;
        status.detail = "未发现 TruckersMP（联运模式检测已关闭）";
    } else {
        std::string evidence;
        switch (lastConvoySession(&evidence)) {
            case LogSession::Running:
                status.kind = MultiplayerKind::Convoy;
                status.detail = "官方 Convoy 会话进行中（依据 game.log.txt 的 [MP] 记录）";
                break;
            case LogSession::None:
                status.kind = MultiplayerKind::None;
                status.detail = "game.log.txt 未发现进行中的联机会话";
                break;
            default:
                status.kind = MultiplayerKind::Unknown;
                status.detail = "读不到 game.log.txt，无法确认是否在官方 Convoy 中"
                                "（已按联机对待，拒绝写入）";
                break;
        }
    }

    cached = status;
    cachedPid = pid;
    cachedAt = now;
    cachedConvoyEnabled = convoyEnabled;
    return status;
}

// ---------------- 联运（官方 Convoy）会话信息 ----------------
bool parseConvoySession(const std::string& logText, ConvoySession* out) {
    if (!out) return false;
    ConvoySession session;
    session.logAvailable = true;

    size_t pos = 0;
    while (pos <= logText.size()) {
        const size_t newline = logText.find('\n', pos);
        const size_t lineEnd = (newline == std::string::npos) ? logText.size() : newline;
        std::string line = logText.substr(pos, lineEnd - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (newline == std::string::npos) pos = logText.size() + 1;
        else pos = newline + 1;

        const size_t mp = line.find("[MP]");
        if (mp == std::string::npos) continue;
        std::string stamp;
        const size_t colon = line.rfind(" : ", mp == std::string::npos ? 0 : mp);
        if (colon != std::string::npos) stamp = line.substr(0, colon);
        std::string body = line.substr(mp + 4);
        while (!body.empty() && body.front() == ' ') body.erase(body.begin());

        if (body.find("Session started.") != std::string::npos ||
            body.find("Game server joined.") != std::string::npos ||
            body.find("Session created") != std::string::npos ||
            body.find("Session running.") != std::string::npos) {
            if (!session.active) {
                session.players.clear();
                if (!stamp.empty()) session.startedAt = stamp;  // 只记第一次进入会话的时间
            }
            session.active = true;
            continue;
        }
        if (body.find("Session closed.") != std::string::npos ||
            body.find("Session closing.") != std::string::npos ||
            body.find("Session closure requested") != std::string::npos) {
            session.active = false;
            session.players.clear();
            continue;
        }
        const bool connected = body.find(" connected, client_id = ") != std::string::npos;
        const bool disconnected = body.find(" disconnected, client_id = ") != std::string::npos;
        if (!connected && !disconnected) continue;

        const std::string key = connected ? " connected, client_id = " : " disconnected, client_id = ";
        const size_t at = body.find(key);
        if (at == std::string::npos) continue;
        ConvoyPlayer player;
        player.name = body.substr(0, at);
        const std::string idText = body.substr(at + key.size());
        player.clientId = ::atoi(idText.c_str());
        player.you = body.find("[you]") != std::string::npos;
        if (disconnected) {
            for (size_t i = 0; i < session.players.size(); ++i) {
                if (session.players[i].clientId == player.clientId &&
                    session.players[i].name == player.name) {
                    session.players.erase(session.players.begin() + (ptrdiff_t)i);
                    break;
                }
            }
            continue;
        }
        bool duplicate = false;
        for (const auto& existing : session.players) {
            if (existing.clientId == player.clientId) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) session.players.push_back(std::move(player));
    }

    session.note = session.active
                       ? fmt("Convoy 会话进行中（自 %s），在线 %d 人",
                             session.startedAt.empty() ? "未知" : session.startedAt.c_str(),
                             (int)session.players.size())
                       : std::string("当前没有进行中的 Convoy 会话");
    *out = std::move(session);
    return true;
}

ConvoySession readConvoySession() {
    ConvoySession session;
    std::string tail;
    if (!readFileTail(documentsDir() + L"\\game.log.txt", 512 * 1024, &tail)) {
        session.logAvailable = false;
        session.note = "读不到 game.log.txt，无法判断 Convoy 会话状态";
        return session;
    }
    parseConvoySession(tail, &session);
    return session;
}

bool isAdmin() {
    BOOL admin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (::AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                   DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        ::CheckTokenMembership(nullptr, group, &admin);
        ::FreeSid(group);
    }
    return admin != FALSE;
}

bool enableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                            &token)) {
        return false;
    }
    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        ::CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    ::CloseHandle(token);
    return ok && ::GetLastError() == ERROR_SUCCESS;
}

// ======================= 窗口 =======================
namespace {

struct FindContext {
    DWORD     pid;
    uintptr_t found;
};

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindContext*>(lparam);
    if (!::IsWindowVisible(hwnd)) return TRUE;
    int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    std::wstring title((size_t)len + 1, L'\0');
    ::GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize((size_t)len);
    if (ctx->pid) {
        DWORD winPid = 0;
        ::GetWindowThreadProcessId(hwnd, &winPid);
        if (winPid != ctx->pid) return TRUE;
    }
    std::wstring low = lower(title);
    if (low.find(L"euro truck simulator 2") != std::wstring::npos ||
        low.find(L"american truck simulator") != std::wstring::npos) {
        ctx->found = (uintptr_t)hwnd;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

uintptr_t findGameWindow(DWORD pid) {
    FindContext ctx{pid, 0};
    ::EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

bool activateWindow(uintptr_t hwndPtr) {
    HWND hwnd = (HWND)hwndPtr;
    if (!hwnd) return false;
    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);

    HWND curFg = ::GetForegroundWindow();
    if (curFg == hwnd) return true;

    DWORD targetThread = ::GetWindowThreadProcessId(hwnd, nullptr);
    DWORD curThread = curFg ? ::GetWindowThreadProcessId(curFg, nullptr) : 0;
    DWORD thisThread = ::GetCurrentThreadId();

    // 规避 Windows 限制：通过模拟 Alt 键让 Windows 允许转移前台焦点
    ::keybd_event(VK_MENU, 0x38, 0, 0);
    ::keybd_event(VK_MENU, 0x38, KEYEVENTF_KEYUP, 0);

    if (targetThread && targetThread != thisThread) {
        ::AttachThreadInput(thisThread, targetThread, TRUE);
    }
    if (curThread && curThread != thisThread && curThread != targetThread) {
        ::AttachThreadInput(curThread, targetThread, TRUE);
    }

    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ::SetForegroundWindow(hwnd);
    ::BringWindowToTop(hwnd);

    typedef void (WINAPI *SwitchToThisWindowProc)(HWND, BOOL);
    HMODULE hUser = ::GetModuleHandleW(L"user32.dll");
    if (hUser) {
        auto pSwitch = (SwitchToThisWindowProc)::GetProcAddress(hUser, "SwitchToThisWindow");
        if (pSwitch) pSwitch(hwnd, TRUE);
    }

    if (curThread && curThread != thisThread && curThread != targetThread) {
        ::AttachThreadInput(curThread, targetThread, FALSE);
    }
    if (targetThread && targetThread != thisThread) {
        ::AttachThreadInput(thisThread, targetThread, FALSE);
    }

    ::Sleep(100);
    return ::GetForegroundWindow() == hwnd;
}

// ======================= 按键注入 =======================
namespace {

const std::map<std::string, int> kControlKeys = {
    {"grave", 0xC0},        {"apostrophe", 0xDE},    {"semicolon", 0xBA},
    {"backslash", 0xDC},    {"bracketleft", 0xDB},   {"bracketright", 0xDD},
    {"minus", 0xBD},        {"equal", 0xBB},         {"comma", 0xBC},
    {"period", 0xBE},       {"slash", 0xBF},         {"space", 0x20},
    {"enter", 0x0D},        {"tab", 0x09},           {"escape", 0x1B},
    {"numpaddivide", 0x6F}, {"numpadmultiply", 0x6A}, {"numpadminus", 0x6D},
    {"numpadplus", 0x6B},   {"numpadenter", 0x0D},   {"numpaddecimal", 0x6E},
    {"backspace", 0x08},
};

bool charToKey(char c, int* vk, bool* shift) {
    if (c >= 'a' && c <= 'z') {
        *vk = c - 'a' + 'A';
        *shift = false;
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *vk = c;
        *shift = true;
        return true;
    }
    if (c >= '0' && c <= '9') {
        *vk = c;
        *shift = false;
        return true;
    }
    struct Pair {
        char  c;
        int   vk;
        bool  shift;
    };
    static const Pair kPairs[] = {
        {' ', 0x20, false}, {'!', 0x31, true},  {'"', 0xDE, true},  {'#', 0x33, true},
        {'$', 0x34, true},  {'%', 0x35, true},  {'&', 0x37, true},  {'\'', 0xDE, false},
        {'(', 0x39, true},  {')', 0x30, true},  {'*', 0x38, true},  {'+', 0xBB, true},
        {',', 0xBC, false}, {'-', 0xBD, false}, {'.', 0xBE, false}, {'/', 0xBF, false},
        {':', 0xBA, true},  {';', 0xBA, false}, {'<', 0xBC, true},  {'=', 0xBB, false},
        {'>', 0xBE, true},  {'?', 0xBF, true},  {'@', 0x32, true},  {'[', 0xDB, false},
        {'\\', 0xDC, false}, {']', 0xDD, false}, {'^', 0x36, true},  {'_', 0xBD, true},
        {'`', 0xC0, false}, {'{', 0xDB, true},  {'|', 0xDC, true},  {'}', 0xDD, true},
        {'~', 0xC0, true},
    };
    for (const auto& p : kPairs) {
        if (p.c == c) {
            *vk = p.vk;
            *shift = p.shift;
            return true;
        }
    }
    return false;
}

void pressKey(int vk, bool shift, int holdMs = 25) {
    BYTE scan = (BYTE)::MapVirtualKeyW((UINT)vk, 0);
    BYTE shiftScan = (BYTE)::MapVirtualKeyW(VK_SHIFT, 0);
    if (shift) ::keybd_event(VK_SHIFT, shiftScan, 0, 0);
    ::keybd_event((BYTE)vk, scan, 0, 0);
    ::Sleep(holdMs);
    ::keybd_event((BYTE)vk, scan, KEYEVENTF_KEYUP, 0);
    if (shift) ::keybd_event(VK_SHIFT, shiftScan, KEYEVENTF_KEYUP, 0);
    ::Sleep(holdMs);
}

}  // namespace

int consoleKeyFromControls(const std::wstring& controlsPath) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(controlsPath, bytes)) return 0;
    std::string text(bytes.begin(), bytes.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line =
            text.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        if (line.find("\"mix console") == std::string::npos) continue;
        size_t k = line.find("keyboard.");
        if (k == std::string::npos) continue;
        size_t start = k + 9;
        size_t end = start;
        while (end < line.size() && ((line[end] >= 'a' && line[end] <= 'z') ||
                                     (line[end] >= 'A' && line[end] <= 'Z') ||
                                     (line[end] >= '0' && line[end] <= '9') ||
                                     line[end] == '_')) {
            ++end;
        }
        std::string name = line.substr(start, end - start);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](char c) { return (char)::tolower(c); });
        auto it = kControlKeys.find(name);
        if (it != kControlKeys.end()) return it->second;
        if (name.size() > 1 && name[0] == 'f') {
            int n = ::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12) return 0x6F + n;
        }
    }
    return 0;
}

SendResult sendConsoleCommand(const std::string& command, int consoleVk, DWORD pid,
                              bool closeAfter) {
    SendResult result;
    uintptr_t hwnd = findGameWindow(pid);
    if (!hwnd) {
        result.message = "没有找到游戏窗口（请先启动欧卡2）";
        return result;
    }
    if (!activateWindow(hwnd)) {
        result.message = "无法把游戏窗口切到前台，请先手动点一下游戏窗口";
        return result;
    }
    // 打开控制台
    pressKey(consoleVk, false, 50);
    ::Sleep(250);

    // 清除控制台输入框可能残存的按键字符（如打开控制台时意外输入的 ` 或 ~）
    pressKey(VK_BACK, false, 25);
    pressKey(VK_BACK, false, 25);
    pressKey(VK_BACK, false, 25);
    ::Sleep(50);

    // 发送命令字符（采用带扫描码的虚拟按键或字符转换，确保 DirectX / DirectInput 正确接收）
    for (char c : command) {
        SHORT vkScan = ::VkKeyScanW(c);
        if (vkScan != -1) {
            BYTE vk = (BYTE)(vkScan & 0xFF);
            BYTE shiftState = (BYTE)((vkScan >> 8) & 0xFF);
            bool needShift = (shiftState & 1) != 0;
            pressKey(vk, needShift, 25);
        } else {
            INPUT input[2] = {};
            input[0].type = INPUT_KEYBOARD;
            input[0].ki.wScan = (WORD)c;
            input[0].ki.dwFlags = KEYEVENTF_UNICODE;

            input[1].type = INPUT_KEYBOARD;
            input[1].ki.wScan = (WORD)c;
            input[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

            ::SendInput(2, input, sizeof(INPUT));
            ::Sleep(25);
        }
        ++result.typed;
    }
    ::Sleep(150);

    // 回车执行
    pressKey(VK_RETURN, false, 50);
    ::Sleep(250);

    // 关闭控制台
    if (closeAfter) {
        pressKey(consoleVk, false, 50);
        ::Sleep(100);
    }
    result.ok = true;
    result.message = "已发送命令: " + command;
    return result;
}

const std::vector<ConsoleKeyOption>& consoleKeyOptions() {
    static const std::vector<ConsoleKeyOption> options = {
        {"~` 键 (默认)", 0xC0},
        {"' \" 引号键", 0xDE},
        {"\\ | 反斜杠键", 0xDC},
        {"; : 分号键", 0xBA},
        {"F12 键", 0x7B},
    };
    return options;
}

// ======================= 飞行模式（自由相机）=======================
int keyNameToVirtualKey(const std::string& name) {
    if (name.size() > 3 && name.compare(0, 3, "key") == 0) {
        const char digit = name[3];
        if (digit >= '0' && digit <= '9') return '0' + (digit - '0');
    }
    if (name.size() > 3 && name.compare(0, 3, "num") == 0 && name[3] >= '0' && name[3] <= '9') {
        if (name.size() == 4) return VK_NUMPAD0 + (name[3] - '0');
        // numdivide / nummultiply / numminus / numplus / numenter / numdecimal
        auto it = kControlKeys.find(name);
        if (it != kControlKeys.end()) return it->second;
    }
    auto it = kControlKeys.find(name);
    if (it != kControlKeys.end()) return it->second;
    if (name.size() > 1 && name[0] == 'f') {
        const int n = ::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) return 0x6F + n;
    }
    return 0;
}

namespace {

// 从一行 mix 定义里取出第一个 keyboard.<name> 按键名。
bool keyboardKeyFromLine(const std::string& text, const std::string& mixName, std::string* key) {
    const std::string needle = "mix " + mixName + " ";
    size_t linePos = 0;
    while (linePos <= text.size()) {
        const size_t newline = text.find('\n', linePos);
        const size_t lineEnd = (newline == std::string::npos) ? text.size() : newline;
        const std::string line = text.substr(linePos, lineEnd - linePos);
        if (line.find(needle) != std::string::npos) {
            const size_t backtick = line.find('`');
            const std::string body = (backtick == std::string::npos) ? line : line.substr(backtick);
            size_t search = 0;
            while (search < body.size()) {
                const size_t at = body.find("keyboard.", search);
                if (at == std::string::npos) break;
                size_t end = at + 9;
                while (end < body.size() && ((body[end] >= 'a' && body[end] <= 'z') ||
                                             (body[end] >= '0' && body[end] <= '9'))) {
                    ++end;
                }
                const std::string name = body.substr(at + 9, end - at - 9);
                if (!name.empty() && name != "rel_position") {
                    *key = name;
                    return true;
                }
                search = end;
            }
        }
        if (newline == std::string::npos) break;
        linePos = newline + 1;
    }
    return false;
}

}  // namespace

bool parseCameraBindings(const std::string& text, CameraBindings* out) {
    if (!out) return false;
    CameraBindings bindings;
    struct Slot {
        const char*    mix;
        int CameraBindings::*field;
    };
    const Slot slots[] = {
        {"camdbg", &CameraBindings::toggle}, {"camfwd", &CameraBindings::forward},
        {"camback", &CameraBindings::back},  {"camleft", &CameraBindings::left},
        {"camright", &CameraBindings::right}, {"camup", &CameraBindings::up},
        {"camdown", &CameraBindings::down},
    };
    int found = 0;
    for (const Slot& slot : slots) {
        std::string name;
        if (!keyboardKeyFromLine(text, slot.mix, &name)) continue;
        const int vk = keyNameToVirtualKey(name);
        if (!vk) continue;
        bindings.*(slot.field) = vk;
        ++found;
    }
    bindings.found = found > 0;
    *out = bindings;
    return bindings.found;
}

bool cameraKeyBindings(const std::wstring& controlsPath, CameraBindings* out) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(controlsPath, bytes) || bytes.empty()) return false;
    const std::string text(bytes.begin(), bytes.end());
    return parseCameraBindings(text, out);
}

SendResult sendKeyTap(int vk, DWORD pid) {
    SendResult result;
    if (!vk) {
        result.message = "按键未识别";
        return result;
    }
    const uintptr_t hwnd = findGameWindow(pid);
    if (!hwnd) {
        result.message = "没有找到游戏窗口（请先启动欧卡2）";
        return result;
    }
    if (!activateWindow(hwnd)) {
        result.message = "无法把游戏窗口切到前台，请先手动点一下游戏窗口";
        return result;
    }
    pressKey(vk, false, 80);
    result.ok = true;
    result.typed = 1;
    result.message = fmt("已发送按键 0x%02X", vk);
    return result;
}

}  // namespace ets2

