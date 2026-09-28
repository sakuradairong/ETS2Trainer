// ui.h —— 界面层：应用状态 + 渲染入口
#pragma once
#include "engine.h"
#include "gameconfig.h"
#include "gameio.h"
#include "memory.h"
#include "saves.h"
#include "vehicle.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ets2 {

enum class PanelKind { Money, Xp, Fuel, Damage, Scanner };

struct AppState {
    // ---------- 进程 ----------
    ProcessMemory                 mem;
    std::unique_ptr<Freezer>      freezer;
    std::unique_ptr<VehicleLocker> vehicleLocker;
    std::unique_ptr<EngineTuner>   engineTuner;
    DWORD                         pid = 0;
    std::wstring                  exeName;
    std::vector<std::wstring>     mpHits;
    bool                          mpBlocks = false;   // TruckersMP / 官方 Convoy
    std::string                   mpDetail;           // 联机判定依据
    std::string                   procStatus = "未附加：请先启动游戏，再点「附加游戏」";
    bool                          attached = false;
    std::string                   memInfo;

    // ---------- 扫描会话 ----------
    std::unique_ptr<ScanSession> moneySession;
    std::unique_ptr<ScanSession> xpSession;
    std::unique_ptr<ScanSession> fuelSession;
    std::unique_ptr<ScanSession> damageSession;
    std::unique_ptr<ScanSession> scanSession;
    VType moneyType = VType::Int64;
    VType xpType = VType::Int32;
    VType fuelType = VType::Float;
    VType damageType = VType::Float;
    VType scanType = VType::Int32;
    char  moneyCurrent[32] = {0};
    char  moneyTarget[32] = "999999999";
    char  moneyTol[16] = "0";
    char  xpCurrent[32] = {0};
    char  xpTarget[32] = "9999999";
    char  xpTol[16] = "0";
    char  fuelCurrent[32] = {0};
    char  fuelTarget[32] = "1500";
    char  fuelTol[16] = "1.0";
    char  damageCurrent[32] = {0};
    char  damageTarget[32] = "0";
    char  damageTol[16] = "0.01";
    char  scanValue[32] = {0};
    char  scanNewValue[32] = {0};
    char  scanTolerance[16] = "0.01";
    int   scanAlign = 4;
    bool  scanIncludeBig = false;
    bool  moneyLock = false;
    bool  xpLock = false;
    bool  fuelLock = false;
    bool  damageLock = false;
    bool  autoFuelLock = false;
    bool  autoDamageLock = false;
    bool  autoAntiRollLock = false;
    float antiRollFactor = 3.0f;       // 1.0=原厂, 2.0=增强稳定, 3.0=不倒翁推荐, 5.0=绝对防翻
    int   enginePowerOption = 0;   // 0=原厂, 1=1.25×, 2=1.50×, 3=2.00×
    bool  engineRaiseLimit = false;
    std::string engineStatus = "未启用。动力调节直接改写发动机数据，不扫描内存。";
    // 上次运行留下的「写入值 / 原厂基准」，用于防止跨进程重启后把放大值当原厂值
    std::string engineStateText;
    std::string moneyStatus = "还没扫描。先在游戏里看一眼现金，填到「当前数值」再扫描。";
    std::string xpStatus = "还没扫描。经验在游戏里是 32 位整数。";
    std::string fuelStatus = "未启用。进入驾驶界面后直接勾选「无限油量」。";
    std::string damageStatus = "未启用。进入驾驶界面后直接勾选「车辆无损」。";
    std::string antiRollStatus = "未启用。方案B：直接改写物理车辆重心/防倾阻尼，极速急转弯不翻车。";
    std::string scanStatus = "未扫描";

    // ---------- 后台任务 ----------
    std::thread           worker;
    std::atomic<bool>     busy{false};
    std::atomic<bool>     cancel{false};
    std::atomic<uint64_t> progressDone{0};
    std::atomic<uint64_t> progressTotal{1};
    std::string           progressNote;
    // 工作线程 -> 界面线程 的通信（避免多线程读写 std::string 冲突）
    std::mutex            resultMutex;
    std::string           pendingStatus;
    std::string           pendingLog;
    std::string           pendingProgressNote;
    int                   pendingKind = 0;
    bool                  pendingReady = false;

    // ---------- 需要用户确认的高风险操作 ----------
    // 0=无, 1=向多个候选地址写入, 2=锁定多个候选地址
    int  pendingMultiAction = 0;
    int  pendingMultiKind = -1;
    bool requestMultiConfirm = false;
    bool requestRestoreConfirm = false;

    // ---------- 游戏内功能 ----------
    char  consoleCmd[256] = "g_set_time 12 0";
    int   consoleKey = 0xC0;
    std::string consoleNote;

    // ---------- 飞行模式（自由相机）----------
    int         flyDeveloper = -1;   // config.cfg 里的 g_developer，-1 = 未知
    int         flyConsole = -1;     // config.cfg 里的 g_console
    std::string flySpeedSaved;       // config.cfg 里的 g_flyspeed
    char        flySpeed[16] = "100";
    std::string flyNote = "还没检查 config.cfg。";
    CameraBindings camera;
    bool        cameraLoaded = false;

    // ---------- 地图传送（控制台 goto）----------
    std::vector<std::string> mapCities;          // 从存档读到的城市内部名
    int         mapCitySelected = -1;
    char        cityFilter[32] = {0};
    char        cityInput[64] = {0};
    char        coordInput[64] = {0};
    char        spotName[32] = {0};
    std::vector<std::pair<std::string, std::string>> teleportSpots;  // 名称 -> goto 目标
    std::string teleportNote = "点「读取城市列表」从存档里取城市名，或直接填城市名 / 坐标。";

    // ---------- 联运模式（Convoy）----------
    // 0 = 严格（联机时全部禁用）；1 = 联运模式（联机时只放开"不写内存"的功能）
    int           mpPolicy = 1;
    bool          checkConvoy = false;  // 是否检测官方联运（默认关闭）
    ConvoySession convoy;
    std::string   convoyNote = "点「刷新联机状态」读取 game.log.txt 的会话与玩家列表。";

    // ---------- 存档 ----------
    std::vector<SaveSlot>   slots;
    std::vector<BackupInfo> backups;
    int           selectedSlot = -1;
    int           selectedBackup = -1;
    std::set<int> scanSelection;
    char          saveMoney[32] = {0};
    char          saveXp[32] = {0};
    std::string   saveNote = "选中一个存档后，这里会显示它的数值情况。";
    TextValues    slotValues;
    bool          showBackupDialog = false;

    // ---------- 设置 ----------
    float maxRegionGb = 1.0f;
    int   workers = 4;
    bool  hotkeysEnabled = true;

    // ---------- 界面 ----------
    int   activeTab = 0;
    bool  requestExit = false;
};

void renderApp(AppState& app);
void initFonts(AppState& app);
void loadSettings(AppState& app);
void saveSettings(const AppState& app);

// 供 UI 调用的动作（都在后台线程里跑，不阻塞界面）
void attachGame(AppState& app);
void detachGame(AppState& app);
void startFirstScan(AppState& app, PanelKind kind);
void startNextScan(AppState& app, PanelKind kind, ScanMode mode);
void writePanelValues(AppState& app, PanelKind kind, bool confirmed = false);
void togglePanelLock(AppState& app, PanelKind kind, bool lock, bool confirmed = false);
void toggleAutoVehicleLock(AppState& app, bool fuel, bool enabled);
void toggleAutoAntiRollLock(AppState& app, bool enabled, float factor);
// option: 0=恢复原厂, 1=1.25×, 2=1.50×, 3=2.00×
void applyEnginePower(AppState& app, int option, bool raiseLimit);

// 无界面自检：确保每个功能面板绑定到自己的输入与锁定状态。
bool validatePanelBindings(std::string* error);

void sendConsole(AppState& app, const std::string& command);
void handleGlobalHotkeys(AppState& app);
void detectConsoleKey(AppState& app);
void checkConsoleConfig(AppState& app);

// ---------- 飞行模式（自由相机）----------
void refreshFlyMode(AppState& app);                        // 读取 config.cfg 状态
void detectCameraKeys(AppState& app);                      // 从 controls.sii 识别相机按键
void enableFlyMode(AppState& app);                         // 写入 config.cfg（需先退出游戏）
void setFlySpeed(AppState& app, const char* value);        // 控制台 g_flyspeed，即时生效
void toggleFreeCamera(AppState& app);                      // 发送 camdbg 按键

// ---------- 地图传送（控制台 goto）----------
void loadMapCities(AppState& app);                         // 从存档解密并提取城市内部名
void teleportTo(AppState& app, const std::string& target); // 发送 goto 命令
void saveTeleportSpot(AppState& app);                      // 把当前目标存成收藏点
void removeTeleportSpot(AppState& app, int index);

// ---------- 联运模式（Convoy）----------
void refreshConvoy(AppState& app);                         // 刷新联机状态与玩家列表
void setMultiplayerPolicy(AppState& app, int policy);      // 0=严格 1=联运模式
// 安全功能（控制台/传送/自由相机）的联机闸门：返回空串表示允许，否则是拒绝原因
std::string multiplayerGateForSafeFeature(const AppState& app);

void refreshSaves(AppState& app);
void selectSlot(AppState& app, int index);
void backupSelectedSlot(AppState& app);
void restoreSelectedBackup(AppState& app);
void exportSelectedSlot(AppState& app);
void applyTextPatch(AppState& app);

}  // namespace ets2
