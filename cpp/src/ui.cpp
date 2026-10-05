// ui.cpp —— ImGui 界面实现
#include "ui.h"

#include "economy.h"
#include "layout.h"
#include "imgui.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ets2 {

namespace {

bool parseNumber(const char* text, bool isFloat, double* out) {
    std::string s(text ? text : "");
    std::string cleaned;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t') continue;
        if (c == '\xEF' || (unsigned char)c >= 0x80) continue;  // 去掉中文逗号等
        cleaned += c;
    }
    if (cleaned.empty()) return false;
    try {
        size_t idx = 0;
        double v = std::stod(cleaned, &idx);
        if (idx != cleaned.size()) return false;
        if (!std::isfinite(v)) return false;
        if (!isFloat) {
            // 扫描 API 内部使用 double 传递输入。超过 2^53 会静默丢失整数精度，
            // 所以明确拒绝，而不是扫描/写入一个不同的值。
            constexpr double kMaxExactInteger = 9007199254740991.0;
            if (std::floor(v) != v || std::fabs(v) > kMaxExactInteger) return false;
        }
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

size_t maxRegionBytes(const AppState& app) {
    if (app.maxRegionGb <= 0.0f) return 0;
    return (size_t)(app.maxRegionGb * 1073741824.0f);
}

ScanSession* sessionFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneySession.get();
    if (kind == PanelKind::Xp) return app.xpSession.get();
    if (kind == PanelKind::Fuel) return app.fuelSession.get();
    if (kind == PanelKind::Damage) return app.damageSession.get();
    return app.scanSession.get();
}

char* currentFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyCurrent;
    if (kind == PanelKind::Xp) return app.xpCurrent;
    if (kind == PanelKind::Fuel) return app.fuelCurrent;
    if (kind == PanelKind::Damage) return app.damageCurrent;
    return app.scanValue;
}

char* targetFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyTarget;
    if (kind == PanelKind::Xp) return app.xpTarget;
    if (kind == PanelKind::Fuel) return app.fuelTarget;
    if (kind == PanelKind::Damage) return app.damageTarget;
    return app.scanNewValue;
}

bool* lockFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return &app.moneyLock;
    if (kind == PanelKind::Xp) return &app.xpLock;
    if (kind == PanelKind::Fuel) return &app.fuelLock;
    if (kind == PanelKind::Damage) return &app.damageLock;
    return nullptr;
}

VType typeFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyType;
    if (kind == PanelKind::Xp) return app.xpType;
    if (kind == PanelKind::Fuel) return app.fuelType;
    if (kind == PanelKind::Damage) return app.damageType;
    return app.scanType;
}

std::string& statusFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyStatus;
    if (kind == PanelKind::Xp) return app.xpStatus;
    if (kind == PanelKind::Fuel) return app.fuelStatus;
    if (kind == PanelKind::Damage) return app.damageStatus;
    return app.scanStatus;
}

//: 浮点扫描容差输入框（整数类型不用）
char* toleranceFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyTol;
    if (kind == PanelKind::Xp) return app.xpTol;
    if (kind == PanelKind::Fuel) return app.fuelTol;
    if (kind == PanelKind::Damage) return app.damageTol;
    return app.scanTolerance;
}

double toleranceValue(AppState& app, PanelKind kind) {
    const char* text = toleranceFor(app, kind);
    double v = ::atof(text ? text : "0");
    return v < 0 ? 0 : v;
}

const char* panelName(PanelKind kind) {
    if (kind == PanelKind::Money) return "现金";
    if (kind == PanelKind::Xp) return "经验";
    if (kind == PanelKind::Fuel) return "油量";
    if (kind == PanelKind::Damage) return "损坏";
    return "扫描器";
}

void resetProcessState(AppState& app) {
    if (app.engineTuner) {
        // 分离前尽力写回原值，然后才放弃内存引用（避免再访问失效的 ProcessMemory）。
        app.engineTuner->restoreAndDetach();
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStateText = app.engineTuner->exportState();
        app.engineStatus = "未启用。动力调节直接改写发动机数据，不扫描内存。";
    }
    if (app.vehicleLocker) app.vehicleLocker->stop();
    if (app.freezer) {
        app.freezer->stop();
        app.freezer->clear();
    }
    app.moneySession.reset();
    app.xpSession.reset();
    app.fuelSession.reset();
    app.damageSession.reset();
    app.scanSession.reset();
    app.scanSelection.clear();
    app.moneyLock = false;
    app.xpLock = false;
    app.fuelLock = false;
    app.damageLock = false;
    app.autoFuelLock = false;
    app.autoDamageLock = false;
    app.moneyStatus = "未附加：请重新扫描现金。";
    app.xpStatus = "未附加：请重新扫描经验。";
    app.fuelStatus = "未附加：无限油量已停止。";
    app.damageStatus = "未附加：车辆无损已停止。";
    app.scanStatus = "未附加：扫描已失效。";
    // 结构化定位结果绑定的地址随进程失效，必须一起清掉
    app.locatedMoneyAddress = 0;
    app.locatedMoneyBank = 0;
    app.locatedMoneyValue = 0;
    app.locatedMoneyDetail.clear();
    app.locatedXpAddress = 0;
    app.locatedEconomy = 0;
    app.locatedXpValue = 0;
    app.locatedXpDetail.clear();
    app.locateStatus = "未定位。填好游戏里的当前金额后点「直接改钱」，程序会自动定位再写入，全程免锁定。";
    app.autoWriteMoneyAfterLocate = false;
    app.autoWriteXpAfterLocate = false;
    app.pendingMultiAction = 0;
    app.pendingMultiKind = -1;
    app.requestMultiConfirm = false;
    app.showCopyDialog = false;
    app.copyRequest = {};
    app.copyName[0] = 0;
    app.showTruckTransfer = false;
    app.showTruckTransferNotice = false;
    app.truckTransferNotice.clear();
    app.truckTransferRequest = {};
    app.transferSourceInventory = {};
    app.transferTargetInventory = {};
    app.transferTargets.clear();
    app.transferTargetIndex = -1;
    app.transferNote.clear();
    app.transferInspectBusy = false; app.transferTargetReady = false;
    ++app.transferInspectionGeneration;
    app.showProfileSplit = false;
    app.splitRows.clear();
    app.splitUiFixture = false;
    { std::lock_guard<std::mutex> lock(app.resultMutex);
      app.pendingReady = false;
      app.pendingLog.clear();
      app.pendingStatus.clear();
      app.pendingProgressNote.clear();
      app.pendingEconomy = PendingEconomyResult{};
      app.pendingCopy = {};
      app.pendingCopyTruck = false;
      app.pendingCopyReady = false; }
    { std::lock_guard<std::mutex> lock(app.resultMutex);
      app.pendingSplit = {}; app.pendingSplitReady = false;
      app.pendingExport = {}; }
}

// ---------- 数值类型下拉框 ----------
bool typeCombo(const char* label, VType* type) {
    static const char* kNames[] = {"int32", "uint32", "int64", "uint64", "float", "double"};
    int current = (int)*type;
    bool changed = false;
    ::ImGui::SetNextItemWidth(90);
    if (::ImGui::BeginCombo(label, kNames[current])) {
        for (int i = 0; i < 6; ++i) {
            bool selected = (i == current);
            if (::ImGui::Selectable(kNames[i], selected)) {
                *type = (VType)i;
                changed = true;
            }
            if (selected) ::ImGui::SetItemDefaultFocus();
        }
        ::ImGui::EndCombo();
    }
    return changed;
}

}  // namespace

bool validatePanelBindings(std::string* error) {
    AppState app;
    const PanelKind kinds[] = {PanelKind::Money, PanelKind::Xp, PanelKind::Fuel,
                               PanelKind::Damage, PanelKind::Scanner};
    char* expectedCurrent[] = {app.moneyCurrent, app.xpCurrent, app.fuelCurrent,
                               app.damageCurrent, app.scanValue};
    char* expectedTarget[] = {app.moneyTarget, app.xpTarget, app.fuelTarget,
                              app.damageTarget, app.scanNewValue};
    bool* expectedLock[] = {&app.moneyLock, &app.xpLock, &app.fuelLock, &app.damageLock, nullptr};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        if (currentFor(app, kinds[i]) != expectedCurrent[i] ||
            targetFor(app, kinds[i]) != expectedTarget[i] ||
            lockFor(app, kinds[i]) != expectedLock[i]) {
            if (error) *error = fmt("面板 %d 的输入/锁定绑定错误", (int)kinds[i]);
            return false;
        }
    }
    double parsed = 0;
    if (!parseNumber("9007199254740991", false, &parsed) ||
        parseNumber("9007199254740992", false, &parsed) || parseNumber("12.5", false, &parsed)) {
        if (error) *error = "整数精度边界校验失败";
        return false;
    }
    return true;
}

// ========================================================================== 
// 工作线程 -> 界面线程 的消息
// ========================================================================== 
namespace {

void postResult(AppState& app, PanelKind kind, const std::string& status,
                const std::string& log = std::string(),
                const std::string& progressNote = std::string()) {
    std::lock_guard<std::mutex> lock(app.resultMutex);
    app.pendingStatus = status;
    app.pendingLog = log;
    app.pendingProgressNote = progressNote;
    app.pendingKind = (int)kind;
    app.pendingReady = true;
}

void applyPending(AppState& app) {
    std::string status, log, progressNote;
    int kind = 0;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingReady) return;
        status = app.pendingStatus;
        log = app.pendingLog;
        progressNote = app.pendingProgressNote;
        kind = app.pendingKind;
        app.pendingStatus.clear();
        app.pendingLog.clear();
        app.pendingProgressNote.clear();
        app.pendingReady = false;
    }
    if (!status.empty()) statusFor(app, (PanelKind)kind) = status;
    if (!log.empty()) logLine(log);
    if (!progressNote.empty()) app.progressNote = progressNote;
}

void joinWorker(AppState& app) {
    if (app.worker.joinable()) app.worker.join();
}

bool ensureAttached(AppState& app) {
    if (app.mem.isOpen() && app.mem.alive()) return true;
    attachGame(app);
    return app.mem.isOpen();
}

}  // namespace

// ==========================================================================
// 进程附加 / 分离
// ==========================================================================
bool validateAttachedSessionReuse(std::string* error) {
    AppState app;
    if (!app.mem.open(::GetCurrentProcessId(), "attached-session-test", error)) return false;
    // 失败时直接返回，绝不让离线测试落入查找/附加真实游戏的分支。
    if (!app.mem.alive()) {
        if (error) *error = "活进程被误判为退出；连续启用会重新附加并重置功能";
        return false;
    }
    app.pid = ::GetCurrentProcessId();
    app.autoFuelLock = app.autoDamageLock = true;
    app.moneyLock = app.xpLock = true;
    app.enginePowerOption = 3;
    app.engineRaiseLimit = true;
    app.locatedMoneyAddress = 123;
    for (int i = 0; i < 10; ++i) {
        if (!ensureAttached(app) || !app.autoFuelLock || !app.autoDamageLock ||
            !app.moneyLock || !app.xpLock ||
            app.enginePowerOption != 3 || !app.engineRaiseLimit ||
            app.locatedMoneyAddress != 123 || app.mem.pid() != ::GetCurrentProcessId()) {
            if (error) *error = "复用当前附加会话时已有功能/定位状态被重置";
            return false;
        }
    }
    return true;
}

static std::string serializeSpots(const AppState& app);
static void parseSpotsInto(const std::string& value, AppState& app);

// 切换目标游戏：必须先停下所有写入线程并恢复现场，再改变选择。
// 任何未确认的发动机恢复凭据都会阻止切换，绝不静默丢弃。
bool selectGame(AppState& app, GameId id, const std::wstring& settingsPath) {
    if (id == selectedGame()) return true;
    app.cancel.store(true);
    joinWorker(app);
    resetProcessState(app);   // 先停动力/车辆/冻结器并清空扫描与地址，才允许换选择
    if (app.engineTuner && (app.engineTuner->active() ||
                            app.engineTuner->hasInvalidatedRecord() ||
                            !app.engineTuner->exportState().empty())) {
        app.procStatus = "切换已取消：发动机原值尚未确认恢复，请先完成恢复再切换游戏。";
        logLine(app.procStatus);
        return false;
    }
    // 先把当前收藏清单归档到所属游戏，再切换选择（设置保存也按游戏分区）
    if (selectedGame() == GameId::Ets2) {
        app.teleportSpotsEts2 = serializeSpots(app);
        app.engineStateEts2 = app.engineStateText;
    } else {
        app.teleportSpotsAts = serializeSpots(app);
        app.engineStateAts = app.engineStateText;
    }
    detachGame(app);
    app.engineStateText.clear();
    setSelectedGame(id);
    app.engineStateText = id == GameId::Ets2 ? app.engineStateEts2 : app.engineStateAts;
    if (app.engineTuner) app.engineTuner->importState(app.engineStateText);
    // 再装入目标游戏的收藏清单
    app.teleportSpots.clear();
    parseSpotsInto(id == GameId::Ets2 ? app.teleportSpotsEts2 : app.teleportSpotsAts, app);
    // 存档/备份/选择等都不允许带进新游戏，否则可能在旧存档上执行新操作
    app.slots.clear();
    app.backups.clear();
    app.selectedSlot = -1;
    app.selectedBackup = -1;
    app.slotValues = {};
    app.telemetry = {};
    app.telemetryAvailable = false;
    app.telemetryCheckedAt = 0;
    app.telemetryError.clear();
    app.saveMoney[0] = app.saveXp[0] = '\0';
    app.saveNote = "请选择当前游戏的存档。";
    app.requestRestoreConfirm = app.showBackupDialog = false;
    app.camera = {};
    app.cameraLoaded = false;
    app.flyDeveloper = app.flyConsole = -1;
    app.flySpeedSaved.clear();
    ::strcpy_s(app.flySpeed, "100");
    app.flyNote = "请重新检查当前游戏的 config.cfg 和相机按键。";
    app.consoleKey = 0xC0;
    ::strcpy_s(app.consoleCmd, "g_set_time 12 0");
    app.consoleNote.clear();
    app.convoy = {};
    app.convoyNote = "请刷新当前游戏的联机状态。";
    app.cityFilter[0] = app.cityInput[0] = app.coordInput[0] = app.spotName[0] = '\0';
    app.teleportNote = "请读取当前游戏的城市列表。";
    app.lastEngineStateRevision = app.lastEngineSaveTick = 0;
    app.mapCities.clear();
    app.mapCitySelected = -1;
    app.mpHits.clear();
    app.memInfo.clear();
    app.procStatus = fmt("已切换到%s：请附加 %s；存档/配置/备份均按该游戏路径读取。",
                         selectedGameDescriptor().shortName,
                         W2U(selectedProcessName()).c_str());
    logLine(app.procStatus);
    if (!supportsVerifiedStructures(selectedGame())) logLine(structuralWriteBlockReason());
    const bool saved = settingsPath.empty() ? saveSettings(app) : saveSettingsTo(app, settingsPath);
    if (!saved) app.procStatus += "（设置保存失败，当前选择仅在本次运行生效）";
    return true;
}

void attachGame(AppState& app) {
    app.cancel.store(true);
    joinWorker(app);
    DWORD pid = 0;
    std::wstring exe;
    if (!findProcess(selectedProcessName(), &pid, &exe)) {
        resetProcessState(app);
        app.mem.close();
        app.procStatus = fmt("没有找到 %s，请先启动游戏", W2U(selectedProcessName()).c_str());
        logLine(app.procStatus);
        app.attached = false;
        // 清掉上一轮的进程/联机痕迹，避免界面显示与当前状态矛盾
        app.pid = 0;
        app.exeName.clear();
        app.mpHits.clear();
        app.memInfo.clear();
        return;
    }
    // 每次重新附加都作为一个新的地址空间处理。即使 PID 相同，也不复用旧扫描与锁定地址。
    resetProcessState(app);
    enableDebugPrivilege();
    std::string err;
    if (!app.mem.open(pid, W2U(exe), &err)) {
        app.procStatus = "附加失败：" + err;
        logLine(app.procStatus);
        app.attached = false;
        app.pid = 0;
        app.exeName.clear();
        app.mpHits.clear();
        app.memInfo.clear();
        return;
    }
    app.pid = pid;
    app.exeName = exe;
    app.attached = true;
    if (!app.freezer) app.freezer = std::make_unique<Freezer>();
    // 锁定（可选保险）同样要过联机闸门：进入联机后不得继续每 250ms 写入
    app.freezer->setWriteGuard([]() { return writeGateReasonFromPolicy(); });
    app.freezer->start(&app.mem, 250);
    if (!app.vehicleLocker) app.vehicleLocker = std::make_unique<VehicleLocker>();
    // 写入闸门：油量/无损的后台周期在真正写入前都会调用它（联机保护）。
    // 不捕获 AppState：worker 线程只读线程安全的策略快照。
    app.vehicleLocker->setWriteGuard([]() { return writeGateReasonFromPolicy(); });
    app.procStatus = fmt("已附加：%s (PID %u)", W2U(exe).c_str(), pid);
    app.mpHits = detectMultiplayer(pid);
    MultiplayerStatus mp = checkMultiplayer(pid, true);
    app.mpBlocks = mp.blocksWrite();
    app.mpDetail = mp.detail;
    if (mp.blocksWrite()) {
        logLine("警告：" + mp.detail + "；联机下改内存可能违反规则，请只在单机使用。");
    }
    std::vector<Region> regions = app.mem.regions(maxRegionBytes(app));
    uint64_t total = 0;
    for (const auto& r : regions) total += r.size;
    app.memInfo = fmt("可写内存：%d 个区域，共 %s", (int)regions.size(),
                      formatSize(total).c_str());
    logLine(app.procStatus);
    logLine(app.memInfo);
    logLine("附加后联机检测：" + mp.detail);
    TelemetrySnapshot telemetry;
    std::string telemetryError;
    if (readTelemetry(&telemetry, &telemetryError))
        logLine(fmt("附加后遥测：game=%u revision=%u fuel=%.2f/%.2f L", telemetry.game,
            telemetry.pluginRevision, telemetry.fuel, telemetry.fuelCapacity));
    else logLine("附加后遥测不可用：" + telemetryError);
}

void detachGame(AppState& app) {
    app.cancel.store(true);
    joinWorker(app);
    resetProcessState(app);
    app.mem.close();
    app.attached = false;
    app.pid = 0;
    app.mpHits.clear();
    app.procStatus = "未附加：请先启动游戏，再点「附加游戏」";
    app.memInfo.clear();
    logLine("已分离进程");
}

void initFonts(AppState& app) {
    (void)app;
    ImGuiIO& io = ::ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    const char* kFonts[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msyh.ttf",
                            "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\Deng.ttf",
                            "C:\\Windows\\Fonts\\simsun.ttc"};
    ImFont* font = nullptr;
    for (const auto* path : kFonts) {
        if (::GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        font = io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg,
                                           io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (font) break;
    }
    if (!font) io.Fonts->AddFontDefault();
}

void loadSettings(AppState& app) {
    loadSettingsFrom(app, applicationDir() + L"\\settings.ini");
}

// 收藏点序列化/解析（切换游戏时按游戏分区保存，互不覆盖）
static std::string serializeSpots(const AppState& app) {
    std::string spots;
    for (const auto& spot : app.teleportSpots) {
        if (!spots.empty()) spots += ",";
        spots += spot.first + "|" + spot.second;
    }
    return spots;
}

static void parseSpotsInto(const std::string& value, AppState& app) {
    app.teleportSpots.clear();
    char separator = ',';
    if (value.find(',') == std::string::npos && value.find(';') != std::string::npos &&
        value.find('|') != value.rfind('|')) {
        separator = ';';
    }
    size_t pos = 0;
    while (pos <= value.size()) {
        const size_t sep = value.find(separator, pos);
        const std::string item =
            value.substr(pos, (sep == std::string::npos) ? std::string::npos : sep - pos);
        const size_t bar = item.find('|');
        if (bar != std::string::npos && bar > 0 && bar + 1 < item.size()) {
            app.teleportSpots.emplace_back(item.substr(0, bar), item.substr(bar + 1));
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
}

void loadSettingsFrom(AppState& app, const std::wstring& path) {
    std::ifstream in{std::filesystem::path(path)};
    if (!in) return;
    std::vector<std::string> lines;
    {
        std::string raw;
        while (std::getline(in, raw)) lines.push_back(raw);
    }
    // 先确定游戏选择，之后的存档目录/收藏等按键都按它解析
    for (const auto& probe : lines) {
        const size_t eqPos = probe.find('=');
        if (eqPos == std::string::npos) continue;
        if (trim(probe.substr(0, eqPos)) == "game") {
            GameId parsed;
            // --game=... 在命令行明确指定时，settings.ini 不得覆盖
            if (!selectionLockedFromCli() &&
                parseGameKey(trim(probe.substr(eqPos + 1)), &parsed)) {
                setSelectedGame(parsed);
            }
        }
    }
    for (const std::string& line : lines) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key == "money_type") vtypeFromName(value.c_str(), &app.moneyType);
        else if (key == "xp_type") vtypeFromName(value.c_str(), &app.xpType);
        else if (key == "fuel_type") vtypeFromName(value.c_str(), &app.fuelType);
        else if (key == "damage_type") vtypeFromName(value.c_str(), &app.damageType);
        else if (key == "money_target") ::strncpy_s(app.moneyTarget, value.c_str(), _TRUNCATE);
        else if (key == "xp_target") ::strncpy_s(app.xpTarget, value.c_str(), _TRUNCATE);
        else if (key == "fuel_target") ::strncpy_s(app.fuelTarget, value.c_str(), _TRUNCATE);
        else if (key == "fuel_tol") ::strncpy_s(app.fuelTol, value.c_str(), _TRUNCATE);
        else if (key == "damage_target") ::strncpy_s(app.damageTarget, value.c_str(), _TRUNCATE);
        else if (key == "damage_tol") ::strncpy_s(app.damageTol, value.c_str(), _TRUNCATE);
        else if (key == "console_key") app.consoleKey = ::atoi(value.c_str());
        else if (key == "max_region_gb") {
            const float parsed = (float)::atof(value.c_str());
            app.maxRegionGb = (std::isfinite(parsed) && parsed >= 0.0f && parsed <= 64.0f)
                                  ? parsed : 1.0f;
        }
        else if (key == "workers") app.workers = ::atoi(value.c_str());
        else if (key == "engine_state" || key == "engine_state_ats") {
            const bool ats = key == "engine_state_ats";
            (ats ? app.engineStateAts : app.engineStateEts2) = value;
            if (ats == (selectedGame() == GameId::Ats)) {
                app.engineStateText = value;
                if (app.engineTuner) app.engineTuner->importState(value);
            }
        }
        else if (key == "fly_speed") ::strncpy_s(app.flySpeed, value.c_str(), _TRUNCATE);
        else if (key == "mp_policy") app.mpPolicy = ::atoi(value.c_str()) == 1 ? 1 : 0;
        else if (key == "check_convoy") app.checkConvoy = (::atoi(value.c_str()) != 0);
        else if (key == "hotkeys_enabled") app.hotkeysEnabled = (::atoi(value.c_str()) != 0);
        else if (key == "teleport_spots" || key == "teleport_spots_ats") {
            // 收藏点按游戏分区：只加载当前选择游戏的键，避免把欧卡2 的收藏带进美卡
            if ((key == "teleport_spots_ats") != (selectedGame() == GameId::Ats)) {
                if (key == "teleport_spots_ats") {
                    app.teleportSpotsAts = value;
                } else {
                    app.teleportSpotsEts2 = value;
                }
                continue;
            }
            // 格式：名字|目标,名字|目标（分隔符必须是 sanitize 白名单外的字符，
            // 否则坐标里的 ';' 会把一个收藏拆碎）
            // 兼容旧版：旧版用 ';' 分隔多条，仅当出现多个 '|' 且没有新分隔符时才按旧规则解析。
            char separator = ',';
            if (value.find(',') == std::string::npos &&
                value.find(';') != std::string::npos && value.find('|') != value.rfind('|')) {
                separator = ';';
            }
            app.teleportSpots.clear();
            size_t pos = 0;
            while (pos <= value.size()) {
                const size_t sep = value.find(separator, pos);
                const std::string item =
                    value.substr(pos, (sep == std::string::npos) ? std::string::npos : sep - pos);
                const size_t bar = item.find('|');
                if (bar != std::string::npos && bar > 0 && bar + 1 < item.size()) {
                    app.teleportSpots.emplace_back(item.substr(0, bar), item.substr(bar + 1));
                }
                if (sep == std::string::npos) break;
                pos = sep + 1;
            }
        }
    }
    setConvoyCheckEnabled(app.checkConvoy);
}

std::string settingsText(const AppState& app) {
    std::string text;
    text += "money_type=" + std::string(vtypeName(app.moneyType)) + "\n";
    text += "xp_type=" + std::string(vtypeName(app.xpType)) + "\n";
    text += "fuel_type=" + std::string(vtypeName(app.fuelType)) + "\n";
    text += "damage_type=" + std::string(vtypeName(app.damageType)) + "\n";
    text += "money_target=" + std::string(app.moneyTarget) + "\n";
    text += "xp_target=" + std::string(app.xpTarget) + "\n";
    text += "fuel_target=" + std::string(app.fuelTarget) + "\n";
    text += "fuel_tol=" + std::string(app.fuelTol) + "\n";
    text += "damage_target=" + std::string(app.damageTarget) + "\n";
    text += "damage_tol=" + std::string(app.damageTol) + "\n";
    text += fmt("console_key=%d\n", app.consoleKey);
    text += fmt("max_region_gb=%g\n", (double)app.maxRegionGb);
    text += fmt("workers=%d\n", app.workers);
    const std::string currentEngineState = app.engineTuner ? app.engineTuner->exportState() : app.engineStateText;
    text += "engine_state=" + (selectedGame() == GameId::Ets2 ? currentEngineState : app.engineStateEts2) + "\n";
    text += "engine_state_ats=" + (selectedGame() == GameId::Ats ? currentEngineState : app.engineStateAts) + "\n";
    text += "fly_speed=" + std::string(app.flySpeed) + "\n";
    text += std::string("game=") + gameKey(selectedGame()) + "\n";
    text += fmt("mp_policy=%d\n", app.mpPolicy);
    text += fmt("check_convoy=%d\n", app.checkConvoy ? 1 : 0);
    text += fmt("hotkeys_enabled=%d\n", app.hotkeysEnabled ? 1 : 0);
    std::string spots;
    for (const auto& spot : app.teleportSpots) {
        // ',' 不在传送目标白名单内，不会被坐标里的 ';' 破坏
        if (!spots.empty()) spots += ",";
        spots += spot.first + "|" + spot.second;
    }
    // 收藏点按游戏分区保存：美卡写 teleport_spots_ats，不覆盖欧卡2 的收藏
    // 两个游戏的收藏都写入；非当前游戏的原样保留，切游戏不会删掉对方的收藏
    if (selectedGame() == GameId::Ats) {
        text += "teleport_spots_ats=" + spots + "\n";
        text += "teleport_spots=" + app.teleportSpotsEts2 + "\n";
    } else {
        text += "teleport_spots=" + spots + "\n";
        text += "teleport_spots_ats=" + app.teleportSpotsAts + "\n";
    }
    return text;
}

bool writeTextFileAtomic(const std::wstring& path, const std::string& text) {
    // 原子写入：先写同目录临时文件再替换，避免中途崩溃/断电留下半截文件。
    const std::wstring tempPath = path + L".tmp";
    {
        std::ofstream out{std::filesystem::path(tempPath), std::ios::trunc | std::ios::binary};
        if (!out) return false;
        out.write(text.data(), (std::streamsize)text.size());
        out.flush();
        if (!out.good()) {
            ::DeleteFileW(tempPath.c_str());
            return false;
        }
    }
    if (!::MoveFileExW(tempPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

bool saveSettingsTo(const AppState& app, const std::wstring& path) {
    // 内容格式与旧版完全一致（键名/顺序不变），可直接覆盖旧文件，无需迁移。
    return writeTextFileAtomic(path, settingsText(app));
}

bool saveSettings(const AppState& app) {
    return saveSettingsTo(app, applicationDir() + L"\\settings.ini");
}

// ==========================================================================
// 写入闸门 + 目标值校验（所有真正写入游戏内存的入口共用）
// ==========================================================================
namespace {

// 测试注入的闸门覆盖实现（默认空：走真实联机检测）
std::mutex       g_writeGateOverrideMutex;
WriteGateOverride g_writeGateOverride;

// 闸门策略快照：只由界面线程写入，工作线程只读原子量 —— 保证 worker 侧的
// 守卫回调不再触碰 AppState 的普通字段（消除跨线程读写竞争）。
struct WriteGatePolicy {
    std::atomic<DWORD> pid{0};
    std::atomic<int>   mpPolicy{1};
};
WriteGatePolicy g_writeGatePolicy;

// 经验业务上限（与 economy.cpp 的写入安全范围一致）
constexpr int32_t kXpBusinessMax = 2000000000;
// 金额安全范围（允许负值＝欠款）
constexpr int64_t kMoneyLimit = 1000000000000LL;

}  // namespace

void setWriteGateOverride(WriteGateOverride override_fn) {
    std::lock_guard<std::mutex> lock(g_writeGateOverrideMutex);
    g_writeGateOverride = std::move(override_fn);
}

void updateWriteGatePolicy(const AppState& app) {
    // 仅界面线程调用：把守卫需要的策略数据放进原子快照
    g_writeGatePolicy.pid.store(app.pid);
    g_writeGatePolicy.mpPolicy.store(app.mpPolicy);
}

namespace {

std::string writeGateReasonInternal(bool forceRefresh) {
    {
        WriteGateOverride override_fn;
        {
            std::lock_guard<std::mutex> lock(g_writeGateOverrideMutex);
            override_fn = g_writeGateOverride;
        }
        if (override_fn) return override_fn();  // 锁外调用，避免持锁执行外部代码
    }
    DWORD pid = g_writeGatePolicy.pid.load();
    if (!pid) {
        std::wstring exe;
        if (!findProcess(selectedProcessName(), &pid, &exe)) {
            // 没有游戏进程：此处不拦（写入本身会以"尚未附加"失败），避免给出误导性的联机拒绝
            return std::string();
        }
    }
    const MultiplayerStatus status = checkMultiplayer(pid, forceRefresh);
    // 写入类功能的既有语义：TruckersMP 一律拒绝；无法确认（Unknown，含模块枚举失败与
    // 读不到 game.log.txt）一律拒绝；Convoy（检测开启时）一律拒绝 —— 与联运模式策略
    // 无关（"不写内存的功能"才受 mpPolicy 影响）。
    if (status.blocksWrite()) return status.detail;
    return std::string();
}

}  // namespace

std::string writeGateReasonFromPolicy() {
    // 任意线程可调用（含 worker 线程）：只读原子快照 + 既有全局开关
    return writeGateReasonInternal(/*forceRefresh=*/false);
}

std::string writeGateReason(AppState& app) {
    // 界面线程的用户操作路径：先同步快照，再强制刷新一次检测结果
    updateWriteGatePolicy(app);
    return writeGateReasonInternal(/*forceRefresh=*/true);
}

bool parseXpTarget(const char* text, int32_t* out, std::string* error) {
    double value = 0.0;
    if (!parseNumber(text, false, &value)) {
        if (error) *error = "「要改成的经验」不是有效数字";
        return false;
    }
    // 先做范围判断，再转换：越界转换是未定义行为，绝不允许
    if (value < (double)INT32_MIN || value > (double)INT32_MAX) {
        if (error) *error = fmt("目标经验超出 32 位整数范围（%lld ~ %lld）", (long long)INT32_MIN,
                                (long long)INT32_MAX);
        return false;
    }
    const int32_t converted = (int32_t)value;
    if ((double)converted != value) {
        if (error) *error = "目标经验必须是整数";
        return false;
    }
    if (converted < 0) {
        if (error) *error = "目标经验不能是负数";
        return false;
    }
    if (converted > kXpBusinessMax) {
        if (error) {
            *error = fmt("目标经验超出安全上限（0 ~ %lld）", (long long)kXpBusinessMax);
        }
        return false;
    }
    if (out) *out = converted;
    if (error) error->clear();
    return true;
}

bool parseMoneyTarget(const char* text, int64_t* out, std::string* error) {
    double value = 0.0;
    if (!parseNumber(text, false, &value)) {
        if (error) *error = "「要改成的金额」不是有效数字";
        return false;
    }
    if (value < -(double)kMoneyLimit || value > (double)kMoneyLimit) {
        if (error) {
            *error = fmt("目标金额超出安全范围（-%lld ~ %lld）", (long long)kMoneyLimit,
                         (long long)kMoneyLimit);
        }
        return false;
    }
    if (out) *out = (int64_t)value;
    if (error) error->clear();
    return true;
}

bool writeBankMoneyGuarded(AppState& app, uint64_t moneyAddress, uint64_t expectedBank,
                           int64_t value, std::string* error) {
    const std::string gate = writeGateReason(app);  // 写入前一刻复核联机状态
    if (!gate.empty()) {
        if (error) *error = "联机保护：" + gate;
        return false;
    }
    return writeBankMoneyVerified(app.mem, moneyAddress, expectedBank, value, error);
}

bool writeXpGuarded(AppState& app, uint64_t xpAddress, uint64_t expectedEconomy, int32_t value,
                    std::string* error) {
    const std::string gate = writeGateReason(app);
    if (!gate.empty()) {
        if (error) *error = "联机保护：" + gate;
        return false;
    }
    return writeEconomyExperienceVerified(app.mem, xpAddress, expectedEconomy, value, error);
}

// ==========================================================================
// 扫描 / 写入 / 锁定
// ==========================================================================
void startFirstScan(AppState& app, PanelKind kind) {
    if (app.busy.load()) {
        statusFor(app, kind) = "已有扫描正在进行，请等它结束或点「取消」";
        return;
    }
    if (!ensureAttached(app)) {
        statusFor(app, kind) = "还没有附加到游戏进程";
        return;
    }
    joinWorker(app);
    applyPending(app);  // 先消费上一轮未显示的结果，防止单槽覆盖
    // 新扫描会使旧候选地址失去语义，先解锁它们，避免后台继续写旧地址。
    if (ScanSession* previous = sessionFor(app, kind); previous && app.freezer) {
        for (uint64_t addr : previous->addresses()) app.freezer->remove(addr);
    }
    if (bool* state = lockFor(app, kind)) *state = false;
    if (kind == PanelKind::Scanner) app.scanSelection.clear();
    VType type = typeFor(app, kind);
    const bool isFloat = vtypeInfo(type).isFloat;
    const char* text = currentFor(app, kind);
    double value = 0.0;
    if (!parseNumber(text, isFloat, &value)) {
        statusFor(app, kind) = "请在「当前数值」里填写一个有效数字（例如 45678）";
        return;
    }
    ScanOptions opt;
    opt.workers = app.workers;
    opt.maxRegionSize = (kind == PanelKind::Scanner && app.scanIncludeBig)
                            ? 0
                            : maxRegionBytes(app);
    const int alignment = (kind == PanelKind::Scanner) ? app.scanAlign : 0;
    auto session =
        std::make_unique<ScanSession>(app.mem, type, alignment, toleranceValue(app, kind));
    ScanSession* raw = session.get();
    if (kind == PanelKind::Money) app.moneySession = std::move(session);
    else if (kind == PanelKind::Xp) app.xpSession = std::move(session);
    else if (kind == PanelKind::Fuel) app.fuelSession = std::move(session);
    else if (kind == PanelKind::Damage) app.damageSession = std::move(session);
    else app.scanSession = std::move(session);

    statusFor(app, kind) =
        fmt("首次扫描 %s = %s 进行中……（约 10~25 秒）", vtypeName(type), text);
    logLine(fmt("[%s] 首次扫描 %s = %s ...", panelName(kind), vtypeName(type), text));
    app.busy.store(true);
    app.cancel.store(false);
    app.progressDone.store(0);
    app.progressTotal.store(1);
    app.progressNote = "首次扫描中";
    const double scanValue = value;
    app.worker = std::thread([&app, raw, opt, scanValue, kind, type]() {
        uint64_t hits = raw->firstScan(
            scanValue, opt,
            [&app](uint64_t done, uint64_t total) {
                app.progressDone.store(done);
                app.progressTotal.store(total ? total : 1);
            },
            [&app]() { return app.cancel.load(); });
        const bool cancelled = app.cancel.load();
        std::string tip;
        if (cancelled) {
            raw->clear();
            hits = 0;
            tip = "已取消，不保留不完整的候选地址。";
        } else if (raw->limitReached()) {
            tip = "命中过多（超过 200 万），已丢弃不完整结果；请缩小扫描范围或避开常见值。";
        } else if (hits == 0) {
            tip = "没找到。请确认数值没填错；也可以在游戏里让数值变化后重扫。";
        } else if (hits == 1) {
            tip = "已收敛到 1 个候选地址，可以写入。";
        } else {
            tip = "还有多个候选：建议继续收窄；强行写入前会再次确认。";
        }
        std::string status = fmt("命中 %llu 处（%s）。%s", (unsigned long long)hits,
                                 vtypeName(type), tip.c_str());
        postResult(app, kind, status,
                   fmt("[%s] 首次扫描完成：命中 %llu 处", panelName(kind),
                       (unsigned long long)hits),
                   cancelled ? "首次扫描已取消" : "首次扫描完成");
        app.busy.store(false);
    });
}

void startNextScan(AppState& app, PanelKind kind, ScanMode mode) {
    if (app.busy.load()) {
        statusFor(app, kind) = "已有扫描正在进行";
        return;
    }
    ScanSession* session = sessionFor(app, kind);
    if (!session || session->empty()) {
        statusFor(app, kind) = "请先做一次「首次扫描」";
        return;
    }
    joinWorker(app);
    applyPending(app);  // 先消费上一轮未显示的结果
    if (kind == PanelKind::Scanner) app.scanSelection.clear();
    const bool isFloat = vtypeInfo(session->type()).isFloat;
    const char* text = currentFor(app, kind);
    double value = 0.0;
    if (mode == ScanMode::Exact && !parseNumber(text, isFloat, &value)) {
        statusFor(app, kind) = "请先在「当前数值」里填写游戏里现在的新数值";
        return;
    }
    app.busy.store(true);
    app.cancel.store(false);
    app.progressDone.store(0);
    app.progressTotal.store(1);
    app.progressNote = "收窄中";
    app.worker = std::thread([&app, session, mode, value, kind]() {
        uint64_t hits = session->nextScan(
            mode, value,
            [&app](uint64_t done, uint64_t total) {
                app.progressDone.store(done);
                app.progressTotal.store(total ? total : 1);
            },
            [&app]() { return app.cancel.load(); });
        const bool cancelled = app.cancel.load();
        std::string status = cancelled
                                 ? "收窄已取消，原候选地址已保留。"
                                 : fmt("收窄后剩余 %llu 处。", (unsigned long long)hits);
        if (!cancelled && hits == 0) status += "可能数值已经变了，建议重新扫描。";
        else if (!cancelled && hits == 1) status += "已收敛到 1 个候选地址。";
        else if (!cancelled && hits > 1) status += "建议继续收窄。";
        postResult(app, kind, status,
                   fmt("[%s] 收窄：剩余 %llu 处", panelName(kind), (unsigned long long)hits),
                   cancelled ? "收窄已取消" : "收窄完成");
        app.busy.store(false);
    });
}

void writePanelValues(AppState& app, PanelKind kind, bool confirmed) {
    if (app.busy.load()) {
        statusFor(app, kind) = "请等当前操作结束";
        return;
    }
    ScanSession* session = sessionFor(app, kind);
    if (!session || session->empty()) {
        statusFor(app, kind) = "请先扫描出地址再写入";
        return;
    }
    const bool isFloat = vtypeInfo(session->type()).isFloat;
    const char* text = targetFor(app, kind);
    double value = 0.0;
    if (!parseNumber(text, isFloat, &value)) {
        statusFor(app, kind) = "请在「要改成」里填写目标数值";
        return;
    }
    const size_t total = session->count();
    if (total > 1 && !confirmed) {
        app.pendingMultiAction = 1;
        app.pendingMultiKind = (int)kind;
        app.requestMultiConfirm = true;
        statusFor(app, kind) =
            fmt("尚有 %d 个候选地址：建议继续收窄，或在确认框中明确批量写入。",
                (int)total);
        return;
    }
    const size_t written =
        isFloat ? session->writeAllNumber(value) : session->writeAllInt((int64_t)value);
    statusFor(app, kind) = fmt("已写入 %d/%d 个地址 = %s（游戏里立刻可见）", (int)written,
                               (int)total, text);
    logLine(fmt("[%s] 写入 %s：成功 %d/%d", panelName(kind), text, (int)written, (int)total));
    bool* lockState = lockFor(app, kind);
    const bool locked = lockState && *lockState;
    if (locked && app.freezer) {
        for (uint64_t addr : session->addresses()) {
            app.freezer->add(addr, session->type(), value);
        }
    }
}

void togglePanelLock(AppState& app, PanelKind kind, bool lock, bool confirmed) {
    ScanSession* session = sessionFor(app, kind);
    if (!session || session->empty() || !app.freezer) {
        if (bool* state = lockFor(app, kind)) *state = false;
        statusFor(app, kind) = "请先扫描出地址再锁定";
        return;
    }
    if (!lock) {
        for (uint64_t addr : session->addresses()) app.freezer->remove(addr);
        statusFor(app, kind) = "已解除锁定。";
        logLine(fmt("[%s] 已解除锁定", panelName(kind)));
        return;
    }
    if (session->count() > 1 && !confirmed) {
        if (bool* state = lockFor(app, kind)) *state = false;
        app.pendingMultiAction = 2;
        app.pendingMultiKind = (int)kind;
        app.requestMultiConfirm = true;
        statusFor(app, kind) =
            fmt("尚有 %d 个候选地址：锁定所有地址前需要确认。", (int)session->count());
        return;
    }
    const bool isFloat = vtypeInfo(session->type()).isFloat;
    const char* text = targetFor(app, kind);
    double value = 0.0;
    if (!parseNumber(text, isFloat, &value)) {
        if (bool* state = lockFor(app, kind)) *state = false;
        statusFor(app, kind) = "请先填写要锁定的目标数值";
        return;
    }
    for (uint64_t addr : session->addresses()) app.freezer->add(addr, session->type(), value);
    statusFor(app, kind) = fmt("已锁定 %d 个地址 = %s（每 0.25 秒写回一次）",
                               (int)session->count(), text);
    logLine(fmt("[%s] 已锁定 %d 个地址", panelName(kind), (int)session->count()));
}

// ==========================================================================
// 游戏内功能（控制台命令）
// ==========================================================================
void sendConsole(AppState& app, const std::string& command) {
    if (command.empty()) return;
    if (app.busy.load()) {
        app.consoleNote = "请先等待当前扫描结束，或取消扫描。";
        return;
    }
    const std::string gate = multiplayerGateForSafeFeature(app);
    if (!gate.empty()) {
        app.consoleNote = gate;
        app.teleportNote = gate;
        logLine("控制台命令被拦截：" + gate);
        return;
    }
    ::strncpy_s(app.consoleCmd, command.c_str(), _TRUNCATE);
    const int vk = app.consoleKey;
    DWORD pid = app.pid;
    if (!pid || !app.mem.alive()) {
        std::wstring exe;
        if (!findProcess(selectedProcessName(), &pid, &exe)) {
            app.consoleNote = fmt("没有找到 %s，请先启动游戏。", W2U(selectedProcessName()).c_str());
            return;
        }
    }
    logLine("正在把命令发送到游戏：" + command);
    joinWorker(app);
    // 上一个工作任务可能已完成但结果尚未在 renderApp 消费，先交付再复用单槽。
    applyPending(app);
    app.busy.store(true);
    app.progressDone.store(0);
    app.progressTotal.store(1);
    app.progressNote = "发送控制台命令中";
    app.worker = std::thread([&app, command, vk, pid]() {
        SendResult result = sendConsoleCommand(command, vk, pid, true);
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingLog = result.message;
            app.pendingStatus.clear();
            app.pendingProgressNote = result.ok ? "控制台命令已发送" : "控制台命令发送失败";
            app.pendingKind = (int)PanelKind::Money;
            app.pendingReady = true;
        }
        app.progressDone.store(1);
        app.busy.store(false);
    });
}

void handleGlobalHotkeys(AppState& app) {
    if (!app.hotkeysEnabled) return;
    // 焦点在任意文本输入框（游戏内或修改器内）时不响应，避免误触发
    if (::ImGui::GetIO().WantTextInput) return;

    static DWORD lastPressTime = 0;
    DWORD now = ::GetTickCount();
    if (now - lastPressTime < 300) return; // 防抖

    bool ctrlDown = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!ctrlDown) return;

    // 边沿触发：每帧无条件刷新按键状态缓存，只有"松开→按下"的那一帧才触发，
    // 长按不会反复翻转开关
    static const int kHotkeys[] = {VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_BACK};
    static bool prevDown[sizeof(kHotkeys) / sizeof(kHotkeys[0])] = {};
    int fired = -1;
    for (size_t i = 0; i < sizeof(kHotkeys) / sizeof(kHotkeys[0]); ++i) {
        const bool down = (::GetAsyncKeyState(kHotkeys[i]) & 0x8000) != 0;
        if (down && !prevDown[i] && fired < 0) fired = (int)i;  // 每帧最多触发一个
        prevDown[i] = down;
    }
    if (fired < 0) return;
    lastPressTime = now;

    switch (kHotkeys[fired]) {
    case VK_F1:  // Ctrl + F1: 传送到导航终点（游戏内再按 Ctrl+F9 落车）
        teleportTo(app, "nav_end");
        logLine("[热键] 传送到导航终点 (goto nav_end)；到位置后按 Ctrl+F9 落车");
        break;
    case VK_F2:  // Ctrl + F2: 切换无限油量
        toggleAutoVehicleLock(app, true, !app.autoFuelLock);
        logLine(std::string("[热键] 无限油量: ") + (app.autoFuelLock ? "开启" : "关闭"));
        break;
    case VK_F3:  // Ctrl + F3: 切换车辆无损
        toggleAutoVehicleLock(app, false, !app.autoDamageLock);
        logLine(std::string("[热键] 车辆无损: ") + (app.autoDamageLock ? "开启" : "关闭"));
        break;
    case VK_F4:  // Ctrl + F4: 切换发动机动力
    {
        int nextOpt = (app.enginePowerOption == 0) ? 3 : 0;
        applyEnginePower(app, nextOpt, nextOpt > 0);
        logLine(std::string("[热键] 发动机动力切换为: ") + (nextOpt == 0 ? "原厂" : "2.0x 狂飙"));
        break;
    }
    case VK_F5:  // Ctrl + F5: 快速把游戏时间设为 09:00
        sendConsole(app, "g_set_time 9 0");
        logLine("[热键] 游戏时间已设为 09:00");
        break;
    case VK_BACK:  // Ctrl + Backspace: 撤销传送
        teleportTo(app, "back");
        logLine("[热键] 撤销传送 (goto back)");
        break;
    default:
        break;
    }
}

void toggleAutoVehicleLock(AppState& app, bool fuel, bool enabled) {
    bool& state = fuel ? app.autoFuelLock : app.autoDamageLock;
    std::string& status = fuel ? app.fuelStatus : app.damageStatus;
    if (!enabled) {
        if (app.vehicleLocker) {
            std::string ignored;
            if (fuel) app.vehicleLocker->setFuelEnabled(false, &ignored);
            else app.vehicleLocker->setDamageEnabled(false, &ignored);
        }
        state = false;
        status = fuel ? "无限油量已关闭。" : "无损锁定已关闭。";
        return;
    }
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        state = false;
        status = reason;
        logLine(reason);
        return;
    }
    if (!ensureAttached(app)) {
        state = false;
        status = "还没有附加到游戏进程";
        return;
    }
    if (!app.mpHits.empty()) {
        state = false;
        status = "检测到联机特征，已拒绝启用车辆锁定；请只在单机使用。";
        return;
    }
    {
        MultiplayerStatus mp = checkMultiplayer(app.pid, true);
        app.mpBlocks = mp.blocksWrite();
        app.mpDetail = mp.detail;
        if (mp.blocksWrite()) {
            state = false;
            status = "写入类功能在联机中始终禁用：" + mp.detail;
            logLine(status);
            return;
        }
    }
    bool other = fuel ? (app.autoDamageLock)
                      : (app.autoFuelLock);
    if (!app.vehicleLocker) app.vehicleLocker = std::make_unique<VehicleLocker>();
    // 写入闸门：油量/无损的后台周期在真正写入前都会调用它（联机保护）。
    // 不捕获 AppState：worker 线程只读线程安全的策略快照。
    app.vehicleLocker->setWriteGuard([]() { return writeGateReasonFromPolicy(); });
    std::string error;
    // 已有另一项锁定时沿用同一条固定指针链；每轮仍会读取当前车辆并做遥测校验。
    if (!other) {
        if (!app.vehicleLocker->bind(&app.mem, app.pid, &error)) {
            state = false;
            status = "自动定位失败：" + error;
            logLine(status);
            return;
        }
    }
    bool ok = fuel ? app.vehicleLocker->setFuelEnabled(true, &error)
                   : app.vehicleLocker->setDamageEnabled(true, &error);
    state = ok;
    status = ok ? app.vehicleLocker->status() : ("自动定位安全校验未通过：" + error);
    logLine(status);
}


// ---------- 发动机动力调节 ----------
namespace {

constexpr float kEngineScales[] = {1.0f, 1.25f, 1.50f, 2.00f};

// 创建（并按需恢复上次运行的记录）动力调节器。
EngineTuner& ensureEngineTuner(AppState& app) {
    if (!app.engineTuner) {
        app.engineTuner = std::make_unique<EngineTuner>();
        if (!app.engineStateText.empty()) app.engineTuner->importState(app.engineStateText);
    }
    return *app.engineTuner;
}

// 运行期间每 5 秒复查一次联机状态；命中即让保持线程恢复原值并停止。
EngineTuner::Guard engineGuardFor(DWORD pid) {
    (void)pid;
    // 与油量/无损/经济写入共用同一套写入闸门（线程安全快照）
    return [](std::string* reason) {
        const std::string blocked = writeGateReasonFromPolicy();
        if (!blocked.empty()) {
            if (reason) *reason = blocked;
            return false;
        }
        return true;
    };
}

std::string multiplayerSummary() {
    if (!isConvoyCheckEnabled()) {
        return "联机保护：拦截 TruckersMP（联运检测已关闭）。";
    }
    return "联机保护：阻止 TruckersMP 与官方 Convoy；状态无法确认时拒绝写入。";
}

}  // namespace

void applyEnginePower(AppState& app, int option, bool raiseLimit) {
    if (!supportsVerifiedStructures(selectedGame())) {
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStatus = structuralWriteBlockReason();
        logLine(app.engineStatus);
        return;
    }
    const int index = (option < 0 || option > 3) ? 0 : option;
    EngineTuner& tuner = ensureEngineTuner(app);

    if (index == 0 && !raiseLimit) {
        std::string error;
        const bool ok = tuner.release(&error);
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStateText = tuner.exportState();
        app.engineStatus = ok ? "已恢复原厂动力" : ("已关闭动力调节（" + error + "）");
        saveSettings(app);
        logLine(app.engineStatus);
        return;
    }

    if (!ensureAttached(app)) {
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStatus = "还没有附加到游戏进程";
        return;
    }
    // 用户操作前用最新状态复核联机（不依赖缓存）
    MultiplayerStatus mp = checkMultiplayer(app.pid, true);
    app.mpBlocks = mp.blocksWrite();
    app.mpDetail = mp.detail;
    if (mp.blocksWrite()) {
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStatus = "已拒绝修改发动机数据：" + mp.detail;
        logLine(app.engineStatus);
        return;
    }

    const bool rebind = !tuner.bound();
    if (rebind) {
        // 只有在没有绑定时才重新绑定，避免丢掉旧对象的原值记录
        tuner.attach(std::make_unique<ProcessMemoryAdapter>(&app.mem),
                     std::make_unique<GameEngineResolver>(&app.mem, app.pid),
                     engineGuardFor(app.pid));
    }
    std::string error;
    const bool ok = tuner.apply(kEngineScales[index], raiseLimit, &error);    app.enginePowerOption = ok ? index : 0;
    app.engineRaiseLimit = ok ? raiseLimit : false;
    app.engineStateText = tuner.exportState();
    app.engineStatus = ok ? tuner.status() : ("动力调节失败：" + error);
    if (ok && mp.kind == MultiplayerKind::Unknown) {
        app.engineStatus += "；注意：无法确认是否在官方 Convoy 中";
    }
    saveSettings(app);
    logLine(app.engineStatus);
}

void detectConsoleKey(AppState& app) {
    std::vector<SaveSlot> slots = app.slots.empty() ? listSlots() : app.slots;
    if (slots.empty()) {
        app.consoleNote = fmt("没有找到%s的 profile，无法自动识别按键", selectedGameDescriptor().shortName);
        return;
    }
    int vk = consoleKeyFromControls(slots.front().profileDir + L"\\controls.sii");
    if (!vk) {
        app.consoleNote = "controls.sii 里没有 `mix console` 绑定，沿用默认的 ~` 键";
        return;
    }
    app.consoleKey = vk;
    app.consoleNote = fmt("已识别控制台按键：0x%02X", vk);
    logLine(app.consoleNote);
    saveSettings(app);
}

void checkConsoleConfig(AppState& app) {
    DWORD runningPid = 0;
    std::wstring runningExe;
    if (findProcess(selectedProcessName(), &runningPid, &runningExe)) {
        app.consoleNote =
            "请先完全退出游戏：游戏退出时会重写 config.cfg，运行中修改会被覆盖（未做任何改动）。";
        logLine(app.consoleNote);
        return;
    }
    std::string text, error;
    if (!readGameConfigText(&text, &error)) {
        app.consoleNote = error;
        return;
    }
    bool changed = false, any = false;
    rewriteConfigKey(&text, "g_console", "1", &changed);
    any |= changed;
    rewriteConfigKey(&text, "g_developer", "1", &changed);
    any |= changed;
    if (!any) {
        app.consoleNote = "config.cfg 检查：g_console=1、g_developer=1，控制台可用。";
        logLine(app.consoleNote);
        return;
    }
    std::wstring backup;
    if (!writeGameConfigText(text, &error, &backup)) {
        app.consoleNote = error;
        logLine(app.consoleNote);
        return;
    }
    app.consoleNote = "已写入 config.cfg（原文件备份为 config.cfg.trainer.bak），重启游戏后生效。";
    logLine(app.consoleNote);
    refreshFlyMode(app);
}

// ==========================================================================
// 飞行模式（自由相机）
// ==========================================================================
namespace {

std::string keyLabel(int vk) {
    if (vk >= '0' && vk <= '9') return fmt("数字键 %c", (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return fmt("小键盘 %c", (char)('0' + (vk - VK_NUMPAD0)));
    for (const auto& option : consoleKeyOptions()) {
        if (option.vk == vk) return option.label;
    }
    return fmt("0x%02X", vk);
}

}  // namespace

void refreshFlyMode(AppState& app) {
    std::string text, error;
    if (!readGameConfigText(&text, &error)) {
        app.flyDeveloper = -1;
        app.flyConsole = -1;
        app.flySpeedSaved.clear();
        app.flyNote = error;
        return;
    }
    std::string value;
    app.flyDeveloper = readConfigKey(text, "g_developer", &value) ? ::atoi(value.c_str()) : -1;
    app.flyConsole = readConfigKey(text, "g_console", &value) ? ::atoi(value.c_str()) : -1;
    const bool hasSpeed = readConfigKey(text, "g_flyspeed", &value);
    app.flySpeedSaved = hasSpeed ? value : std::string("(未设置)");
    if (!hasSpeed) {
        app.flyNote = "config.cfg 里没有 g_flyspeed，点「写入飞行模式设置」会补上。";
    } else if (app.flyDeveloper == 1 && app.flyConsole == 1) {
        app.flyNote = fmt("config.cfg 已就绪：g_developer=1、g_console=1、g_flyspeed=%s。",
                          app.flySpeedSaved.c_str());
    } else {
        app.flyNote = fmt("config.cfg：g_developer=%d、g_console=%d、g_flyspeed=%s；"
                          "需要开启后才能用自由相机。",
                          app.flyDeveloper, app.flyConsole, app.flySpeedSaved.c_str());
    }
    if (hasSpeed) {
        ::strncpy_s(app.flySpeed, value.c_str(), _TRUNCATE);
    }
}

void detectCameraKeys(AppState& app) {
    std::vector<SaveSlot> slots = app.slots.empty() ? listSlots() : app.slots;
    if (slots.empty()) {
        app.flyNote = fmt("没有找到%s的 profile，无法识别相机按键", selectedGameDescriptor().shortName);
        return;
    }
    CameraBindings bindings;
    if (!cameraKeyBindings(slots.front().profileDir + L"\\controls.sii", &bindings)) {
        app.flyNote = "controls.sii 里没有找到 mix cam* 绑定（沿用默认：数字键 0 开关，小键盘 8/5/4/6/9/3）";
        return;
    }
    app.camera = bindings;
    app.cameraLoaded = true;
    app.flyNote = fmt("已识别相机按键：开关 %s、前后 %s/%s、左右 %s/%s、上下 %s/%s",
                      keyLabel(bindings.toggle).c_str(), keyLabel(bindings.forward).c_str(),
                      keyLabel(bindings.back).c_str(), keyLabel(bindings.left).c_str(),
                      keyLabel(bindings.right).c_str(), keyLabel(bindings.up).c_str(),
                      keyLabel(bindings.down).c_str());
    logLine(app.flyNote);
}

void enableFlyMode(AppState& app) {
    DWORD runningPid = 0;
    std::wstring runningExe;
    if (findProcess(selectedProcessName(), &runningPid, &runningExe)) {
        app.flyNote = "请先完全退出游戏：游戏退出时会重写 config.cfg，运行中修改会被覆盖（未做任何改动）。";
        logLine(app.flyNote);
        return;
    }
    std::string text, error;
    if (!readGameConfigText(&text, &error)) {
        app.flyNote = error;
        return;
    }
    const std::string speed = app.flySpeed[0] ? app.flySpeed : "100";
    bool changed = false, any = false;
    rewriteConfigKey(&text, "g_developer", "1", &changed);
    any |= changed;
    rewriteConfigKey(&text, "g_console", "1", &changed);
    any |= changed;
    rewriteConfigKey(&text, "g_flyspeed", speed, &changed);
    any |= changed;
    if (!any) {
        app.flyNote = "config.cfg 已经满足飞行模式要求，无需修改。";
        refreshFlyMode(app);
        return;
    }
    std::wstring backup;
    if (!writeGameConfigText(text, &error, &backup)) {
        app.flyNote = error;
        logLine(app.flyNote);
        return;
    }
    app.flyNote = fmt("已写入 config.cfg（备份 %s）。重启游戏后进驾驶界面：按 %s 开关自由相机。",
                      W2U(backup).c_str(),
                      app.cameraLoaded ? keyLabel(app.camera.toggle).c_str() : "数字键 0");
    logLine(app.flyNote);
    refreshFlyMode(app);
}

void setFlySpeed(AppState& app, const char* value) {
    if (!value || !value[0]) return;
    ::strncpy_s(app.flySpeed, value, _TRUNCATE);
    sendConsole(app, std::string("g_flyspeed ") + value);
}

void toggleFreeCamera(AppState& app) {
    const std::string gate = multiplayerGateForSafeFeature(app);
    if (!gate.empty()) {
        app.flyNote = gate;
        logLine("自由相机被拦截：" + gate);
        return;
    }
    DWORD pid = app.pid;
    if (!pid) {
        std::wstring exe;
        if (!findProcess(selectedProcessName(), &pid, &exe)) {
            app.flyNote = fmt("没有找到 %s，请先启动游戏。", W2U(selectedProcessName()).c_str());
            return;
        }
    }
    const int vk = app.cameraLoaded && app.camera.toggle ? app.camera.toggle : '0';
    const SendResult result = sendKeyTap(vk, pid);
    app.flyNote = result.message + (result.ok ? "（自由相机开关）" : "");
    logLine(app.flyNote);
}

// ==========================================================================
// 存档工具
// ==========================================================================
void refreshSaves(AppState& app) {
    app.slots = listSlots();
    app.backups = listBackups();
    int scs = 0, text = 0;
    for (const auto& s : app.slots) {
        if (s.format == "scs") ++scs;
        else if (s.format == "text") ++text;
    }
    app.saveNote = fmt("共找到 %d 个存档槽（%d 个加密、%d 个明文）。选一个看看它的数值。",
                       (int)app.slots.size(), scs, text);
    if (app.selectedSlot >= (int)app.slots.size()) app.selectedSlot = -1;
    logLine(fmt("存档列表已刷新，共 %d 个存档槽", (int)app.slots.size()));
}

void selectSlot(AppState& app, int index) {
    if (index < 0 || index >= (int)app.slots.size()) return;
    app.selectedSlot = index;
    const SaveSlot& slot = app.slots[(size_t)index];
    app.slotValues = {};
    if (slot.format != "text") {
        app.saveMoney[0] = 0;
        app.saveXp[0] = 0;
        app.saveNote = fmt("%s 是 %s 格式。加密/二进制存档本工具不写；想改钱请到「实时改钱」"
                           "改内存，然后让游戏自动存档。",
                           slot.label.c_str(), slot.formatText.c_str());
        return;
    }
    TextValues values;
    std::string err;
    if (readTextValues(slot.gameSii, &values, &err)) {
        app.slotValues = values;
        if (values.hasMoney) ::strncpy_s(app.saveMoney, formatInt(values.money).c_str(), _TRUNCATE);
        if (values.hasExperience) {
            ::strncpy_s(app.saveXp, formatInt(values.experience).c_str(), _TRUNCATE);
        }
        app.saveNote = fmt("%s：明文存档，读到 现金=%s、经验=%s。改完点「写入明文存档」。",
                           slot.label.c_str(),
                           values.hasMoney ? formatInt(values.money).c_str() : "无",
                           values.hasExperience ? formatInt(values.experience).c_str() : "无");
    } else {
        app.saveNote = fmt("%s：读取数值失败（%s）", slot.label.c_str(), err.c_str());
    }
}

void consumePendingCopy(AppState& app) {
    SaveCopyResult result;
    bool truck = false;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingCopyReady) return;
        result = std::move(app.pendingCopy);
        truck = app.pendingCopyTruck;
        app.pendingCopyTruck = false;
        app.pendingCopyReady = false;
    }
    if (result.game != selectedGame()) return;
    if (result.ok) {
        refreshSaves(app);
        for (int i = 0; i < (int)app.slots.size(); ++i)
            if (::_wcsicmp(app.slots[i].slotDir.c_str(), result.slotDir.c_str()) == 0) { selectSlot(app, i); break; }
        app.saveNote = std::string(truck ? "卡车已导入新存档「" : "已复制为「") + result.displayName + "」，新槽位：" + W2U(result.slotName) +
            "。启动游戏后在同一档案的载入列表中查看；云存档请等待 Steam 同步完成。";
    } else app.saveNote = std::string(truck ? "车辆导入失败：" : "复制失败：") + result.error;
    logLine(app.saveNote);
}

void consumePendingSplit(AppState& app) {
    ProfileSplitResult result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingSplitReady) return;
        result = std::move(app.pendingSplit); app.pendingSplitReady = false;
    }
    if (result.game != selectedGame()) return;
    if (!result.created.empty()) refreshSaves(app);
    std::string names;
    for (const auto& profile : result.created) {
        if (!names.empty()) names += "、";
        names += "「" + profile.name + "」";
    }
    if (result.ok) app.saveNote = "已生成独立本地档案：" + names +
        "。启动游戏后选择对应档案，检查模组管理器，再载入保留的存档。";
    else app.saveNote = "拆分未全部完成：" + result.error +
        (names.empty() ? "。未生成新档案。" : "。已生成并保留：" + names + "；其余项目请更换名称后重试。");
    logLine(app.saveNote);
}

void openProfileSplit(AppState& app) {
    if (app.busy || app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "请先选中 TMP、ProMods 或联运所在档案的一个存档"; return;
    }
    std::string error;
    if (copyBlockedByRunningGame(selectedGame(), &error)) { app.saveNote = error; return; }
    const auto source = app.slots[app.selectedSlot];
    app.splitRows.clear(); app.splitGame = selectedGame(); app.splitUiFixture = false;
    for (const auto& slot : app.slots) if (::_wcsicmp(slot.profileDir.c_str(), source.profileDir.c_str()) == 0) {
        ProfileSplitRow row; row.source = slot;
        row.selected = ::_wcsicmp(slot.slotDir.c_str(), source.slotDir.c_str()) == 0;
        const auto name = slot.displayName.empty() ? W2U(slot.profileName) + "-" + W2U(slot.slotName) : slot.displayName;
        ::strncpy_s(row.name, name.c_str(), _TRUNCATE); app.splitRows.push_back(row);
    }
    app.showProfileSplit = true;
}

void startProfileSplit(AppState& app) {
    if (app.busy || app.splitUiFixture) return;
    joinWorker(app); consumePendingCopy(app); consumePendingSplit(app);
    ProfileSplitRequest request; request.game = app.splitGame;
    for (const auto& row : app.splitRows) if (row.selected) request.items.push_back({row.source, row.name});
    app.busy = true; app.cancel = false; app.saveNote = "正在准备独立档案并校验完整存档……";
    app.worker = std::thread([&app, request] {
        ProfileSplitResult result;
        try { result = splitSelectedProfiles(request, [&app] { return app.cancel.load(); }); }
        catch (const std::exception& e) { result.game = request.game; result.error = e.what(); }
        { std::lock_guard<std::mutex> lock(app.resultMutex);
          app.pendingSplit = std::move(result); app.pendingSplitReady = true; }
        app.busy = false;
    });
}

void renderProfileSplitDialog(AppState& app) {
    if (app.showProfileSplit) { ::ImGui::OpenPopup("拆分为独立档案##split"); app.showProfileSplit = false; }
    ::ImGui::SetNextWindowSize(ImVec2(780, 0), ImGuiCond_Appearing);
    if (!::ImGui::BeginPopupModal("拆分为独立档案##split", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (app.splitRows.empty() || app.splitGame != selectedGame()) {
        ::ImGui::CloseCurrentPopup(); ::ImGui::EndPopup(); return;
    }
    ::ImGui::Text("%s / 源档案：%s", gameDescriptor(app.splitGame).shortName,
                  W2U(app.splitRows.front().source.profileName).c_str());
    ::ImGui::TextWrapped("勾选需要分离的存档，每个存档填写一个不同的新档案名（例如 TMP、ProMods、联运）。最多选择 16 个。");
    ::ImGui::TextWrapped("每个新档案仅保留选中的存档，继承当前设置、控制器和模组配置。历史模组顺序无法自动还原；首次载入前请检查模组管理器。");
    ::ImGui::TextWrapped("生成到本地 profiles；支持加密进度，原档案保留。Steam 云源的本地控制器设置会一起复制。");
    ::ImGui::BeginChild("##split-list", ImVec2(0, 270), ImGuiChildFlags_Borders);
    if (::ImGui::BeginTable("##split-rows", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ::ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 45);
        ::ImGui::TableSetupColumn("槽位", ImGuiTableColumnFlags_WidthFixed, 105);
        ::ImGui::TableSetupColumn("存档名称"); ::ImGui::TableSetupColumn("新档案名称"); ::ImGui::TableHeadersRow();
        for (int i = 0; i < (int)app.splitRows.size(); ++i) {
            auto& row = app.splitRows[i]; ::ImGui::PushID(i); ::ImGui::TableNextRow();
            ::ImGui::TableSetColumnIndex(0); ::ImGui::Checkbox("##enabled", &row.selected);
            ::ImGui::TableSetColumnIndex(1); ::ImGui::TextUnformatted(W2U(row.source.slotName).c_str());
            ::ImGui::TableSetColumnIndex(2); ::ImGui::TextUnformatted(row.source.displayName.c_str());
            ::ImGui::TableSetColumnIndex(3); ::ImGui::BeginDisabled(!row.selected);
            ::ImGui::SetNextItemWidth(-1); ::ImGui::InputText("##name", row.name, sizeof(row.name));
            ::ImGui::EndDisabled(); ::ImGui::PopID();
        }
        ::ImGui::EndTable();
    }
    ::ImGui::EndChild();
    size_t count = 0; std::set<std::wstring> names; std::string error;
    for (const auto& row : app.splitRows) if (row.selected) {
        ++count; std::wstring folder; std::string why;
        if (!profileFolderName(row.name, &folder, &why)) { if (error.empty()) error = why; }
        else if (!names.insert(folder).second) error = "新档案名称不能重复";
    }
    if (!count || count > 16) error = "请选择 1 至 16 个存档";
    ::ImGui::Text("将创建 %zu 个独立档案", count);
    if (!error.empty()) ::ImGui::TextWrapped("%s", error.c_str());
    ::ImGui::TextDisabled("同名档案会拒绝覆盖。若中途失败，结果会列出已生成的档案。");
    ::ImGui::BeginDisabled(!error.empty() || app.splitUiFixture);
    if (::ImGui::Button("生成独立档案")) { startProfileSplit(app); ::ImGui::CloseCurrentPopup(); }
    ::ImGui::EndDisabled(); ::ImGui::SameLine();
    if (::ImGui::Button("取消")) ::ImGui::CloseCurrentPopup();
    ::ImGui::EndPopup();
}

void startSaveCopy(AppState& app) {
    if (app.busy) return;
    joinWorker(app);
    consumePendingCopy(app);
    SaveCopyRequest request = app.copyRequest;
    request.displayName = app.copyName;
    app.cancel = false;
    app.busy = true;
    app.saveNote = "正在复制并校验存档文件……";
    app.worker = std::thread([&app, request] {
        SaveCopyResult result;
        try { result = copySlotToNewSave(request, [&app] { return app.cancel.load(); }); }
        catch (const std::exception& e) { result.game = request.game; result.error = e.what(); }
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingCopy = std::move(result);
            app.pendingCopyTruck = false;
            app.pendingCopyReady = true;
        }
        app.busy = false;
    });
}


void consumePendingTransferInspection(AppState& app) {
    PendingTransferInspection result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if(!app.pendingTransferInspection.ready) return;
        result=std::move(app.pendingTransferInspection);
        app.pendingTransferInspection={};
    }
    if(result.game!=selectedGame() || result.generation!=app.transferInspectionGeneration) return;
    app.transferInspectBusy=false;
    app.progressDone=1;
    if(result.target) {
        app.transferTargetReady=result.ok;
        app.transferTargetInventory=result.ok ? std::move(result.inventory) : TransferInventory{};
        app.transferNote=!result.ok ? "目标无法使用："+result.error :
            app.transferTargetInventory.garages.empty() ? "目标没有车辆和司机均为空的车库位置。" :
            "目标版本和依赖检查通过。请选择车库和新存档名称。";
    } else {
        app.transferSourceInventory=result.ok ? std::move(result.inventory) : TransferInventory{};
        app.transferNote=!result.ok ? "无法读取源车辆："+result.error :
            app.transferSourceInventory.trucks.empty() ? "源存档没有可复制的自有卡车。" :
            app.transferTargets.empty() ? "没有其他目标存档，请先在目标档案创建一个可用存档。" :
            "源车辆已读取。请选择卡车和目标存档。";
    }
}

void startTransferInspection(AppState& app,bool target) {
    if(app.busy) return;
    joinWorker(app);
    const auto request=app.truckTransferRequest;
    const uint64_t generation=++app.transferInspectionGeneration;
    app.transferInspectBusy=true; app.cancel=false; app.busy=true;
    app.progressDone=0; app.progressTotal=1;
    app.progressNote=target ? "读取目标车库并检查兼容性" : "读取源车辆";
    app.transferNote=target ? "正在自动解密和转换目标存档，并检查版本与依赖……" :
                             "正在自动解密和转换源存档，请稍候……";
    app.worker=std::thread([&app,request,generation,target]{
        PendingTransferInspection result;
        result.game=request.game; result.generation=generation; result.target=target;
        auto canceled=[&app]{return app.cancel.load();};
        result.ok=target ? inspectTruckTransferTarget(request.source,request.target,&result.inventory,&result.error,canceled) :
                           inspectTruckTransferSlot(request.source,&result.inventory,&result.error,canceled);
        result.ready=true;
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingTransferInspection=std::move(result);
        }
        app.busy=false;
    });
}

void openTruckTransfer(AppState& app) {
    auto explain=[&](const std::string& message) {
        app.saveNote=message;
        app.truckTransferNotice=message;
        app.showTruckTransferNotice=true;
        logLine("跨存档复制卡车："+message);
    };
    if(app.busy) {explain("当前任务正在处理中，请完成或取消后再复制卡车。");return;}
    std::string error;
    if(copyBlockedByRunningGame(selectedGame(),&error)){explain(error);return;}
    if(app.selectedSlot<0 || app.selectedSlot>=(int)app.slots.size()) {
        explain("请先在下方存档列表中选中提供卡车的源存档，再点击「跨存档复制卡车」。");return;
    }
    app.truckTransferRequest={};
    app.truckTransferRequest.game=selectedGame();
    app.truckTransferRequest.source=app.slots[app.selectedSlot];
    app.transferSourceInventory={};
    app.transferTargets.clear();
    for(const auto& slot:app.slots)
        if(::_wcsicmp(slot.slotDir.c_str(),app.truckTransferRequest.source.slotDir.c_str())!=0)
            app.transferTargets.push_back(slot);
    app.transferTruckIndex=0;app.transferTargetIndex=-1;app.transferGarageIndex=0;
    app.transferTargetInventory={};
    app.transferTargetReady=false;
    ::strncpy_s(app.transferName,"车辆导入副本",_TRUNCATE);
    app.showTruckTransfer=true;
    logLine("跨存档复制卡车：打开选择窗口并读取源车辆。");
    startTransferInspection(app,false);
}

void startTruckTransfer(AppState& app) {
    if(app.busy || !app.transferTargetReady || app.transferTruckIndex<0 ||
       app.transferTruckIndex>=(int)app.transferSourceInventory.trucks.size() ||
       app.transferGarageIndex<0 || app.transferGarageIndex>=(int)app.transferTargetInventory.garages.size()) return;
    joinWorker(app);consumePendingCopy(app);
    TruckTransferRequest request=app.truckTransferRequest;
    request.truckId=app.transferSourceInventory.trucks.at(app.transferTruckIndex).id;
    request.garageId=app.transferTargetInventory.garages.at(app.transferGarageIndex).id;
    request.displayName=app.transferName;
    app.busy=true;app.cancel=false;
    app.progressDone=0;app.progressTotal=1;app.progressNote="转换并复制卡车到新存档";
    app.saveNote="正在校验卡车配件和目标存档，并创建车辆导入副本……";
    app.worker=std::thread([&app,request]{
        auto result=transferTruckToNewSave(request,[&app]{return app.cancel.load();});
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingCopy=std::move(result);app.pendingCopyTruck=true;app.pendingCopyReady=true;
        }
        app.busy=false;
    });
}

void renderTruckTransferDialog(AppState& app) {
    const char* notice="跨存档复制卡车提示##transfer_notice";
    if(app.showTruckTransferNotice) {::ImGui::OpenPopup(notice);app.showTruckTransferNotice=false;}
    ::ImGui::SetNextWindowSize(ImVec2(560,0),ImGuiCond_Appearing);
    if(::ImGui::BeginPopupModal(notice,nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ::ImGui::TextWrapped("%s",app.truckTransferNotice.c_str());
        if(app.truckTransferUiProbe.capture) app.truckTransferUiProbe.noticeVisible=true;
        if(::ImGui::Button("知道了")) ::ImGui::CloseCurrentPopup();
        ::ImGui::EndPopup();
    }
    if(app.showTruckTransfer){::ImGui::OpenPopup("跨存档复制卡车##transfer");app.showTruckTransfer=false;}
    ::ImGui::SetNextWindowSize(ImVec2(740,0),ImGuiCond_Appearing);
    if(!::ImGui::BeginPopupModal("跨存档复制卡车##transfer",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(app.truckTransferRequest.source.slotDir.empty() ||
       app.truckTransferRequest.game!=selectedGame()){
        ::ImGui::CloseCurrentPopup();::ImGui::EndPopup();return;
    }
    ::ImGui::TextWrapped("源：%s / %s",gameDescriptor(app.truckTransferRequest.game).shortName,
                        app.truckTransferRequest.source.label.c_str());
    ::ImGui::TextWrapped("自动解密并读取现有存档，无需设置游戏存档格式。复制到目标档案的新存档槽，保留两份原档。需同游戏、同存档版本及兼容的 DLC／模组。");
    ::ImGui::BeginDisabled(app.busy);
    ::ImGui::SetNextItemWidth(650);
    const auto& trucks=app.transferSourceInventory.trucks;
    if(::ImGui::BeginCombo("卡车",trucks.empty()?"无车辆":trucks[app.transferTruckIndex].label.c_str())){
        for(int i=0;i<(int)trucks.size();++i){
            ::ImGui::PushID(i);
            auto label=trucks[i].label+" / "+trucks[i].id+" / "+std::to_string(trucks[i].accessories)+" 配件";
            if(::ImGui::Selectable(label.c_str(),i==app.transferTruckIndex))app.transferTruckIndex=i;
            ::ImGui::PopID();
        }
        ::ImGui::EndCombo();
    }
    ::ImGui::SetNextItemWidth(650);
    if(::ImGui::BeginCombo("目标存档",app.transferTargetIndex<0?"请选择目标存档":app.transferTargets[app.transferTargetIndex].label.c_str())){
        for(int i=0;i<(int)app.transferTargets.size();++i){
            ::ImGui::PushID(i);
            if(::ImGui::Selectable(app.transferTargets[i].label.c_str(),i==app.transferTargetIndex)){
                app.transferTargetIndex=i;app.transferGarageIndex=0;
                app.truckTransferRequest.target=app.transferTargets[i];
                app.transferTargetInventory={}; app.transferTargetReady=false;
                startTransferInspection(app,true);
            }
            ::ImGui::PopID();
        }
        ::ImGui::EndCombo();
    }
    const auto& garages=app.transferTargetInventory.garages;
    ::ImGui::SetNextItemWidth(650);
    if(::ImGui::BeginCombo("目标车库",garages.empty()?"无可用空位":garages[app.transferGarageIndex].id.c_str())){
        for(int i=0;i<(int)garages.size();++i){
            const auto label=garages[i].id+"（"+std::to_string(garages[i].freeSlots)+" 个空位）";
            if(::ImGui::Selectable(label.c_str(),i==app.transferGarageIndex))app.transferGarageIndex=i;
        }
        ::ImGui::EndCombo();
    }
    ::ImGui::SetNextItemWidth(500);
    ::ImGui::InputText("新存档名称",app.transferName,sizeof(app.transferName));
    ::ImGui::EndDisabled();
    ::ImGui::TextWrapped("%s",app.transferNote.c_str());
    std::string escaped,error;
    bool valid=siiEscapeName(app.transferName,&escaped,&error);
    if(!valid)::ImGui::TextWrapped("%s",error.c_str());
    ::ImGui::BeginDisabled(!valid || garages.empty() || trucks.empty() || app.transferTargetIndex<0 ||
                           !app.transferTargetReady || app.transferInspectBusy || app.busy);
    if(::ImGui::Button("复制卡车并另存为新槽位")){startTruckTransfer(app);::ImGui::CloseCurrentPopup();}
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    if(::ImGui::Button(app.transferInspectBusy ? "取消读取并关闭" : "关闭")) {
        if(app.transferInspectBusy) app.cancel=true;
        app.transferInspectBusy=false; app.transferTargetReady=false;
        ++app.transferInspectionGeneration;
        ::ImGui::CloseCurrentPopup();
    }
    ::ImGui::EndPopup();
}


void backupSelectedSlot(AppState& app) {
    if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "请先在列表里选中一个存档";
        return;
    }
    const SaveSlot& slot = app.slots[(size_t)app.selectedSlot];
    std::string err;
    std::wstring dir = backupSlot(slot, "manual", &err);
    if (dir.empty()) {
        app.saveNote = "备份失败：" + err;
        logLine(app.saveNote);
        return;
    }
    app.saveNote = "已备份到：" + W2U(dir);
    logLine("已备份 " + slot.label + " -> " + W2U(dir));
    app.backups = listBackups();
}

void restoreSelectedBackup(AppState& app) {
    if (app.selectedBackup < 0 || app.selectedBackup >= (int)app.backups.size()) {
        app.saveNote = "请先在「备份列表」里选中一个备份";
        return;
    }
    app.requestRestoreConfirm = true;
    app.saveNote = "还原会覆盖当前存档，请在确认框中核对。";
}

namespace {

void performRestoreSelectedBackup(AppState& app) {
    if (app.selectedBackup < 0 || app.selectedBackup >= (int)app.backups.size()) {
        app.saveNote = "选中的备份已失效，请重新选择";
        return;
    }
    DWORD gamePid = 0;
    if (findProcess(selectedProcessName(), &gamePid, nullptr)) {
        app.saveNote = fmt("还原已取消：请先退出%s，避免游戏或 Steam 云同步覆盖文件。", selectedGameDescriptor().shortName);
        return;
    }
    const BackupInfo info = app.backups[(size_t)app.selectedBackup];
    std::vector<SaveSlot> currentSlots = listSlots();
    const SaveSlot* current = nullptr;
    for (const auto& slot : currentSlots) {
        if (_wcsicmp(slot.slotDir.c_str(), info.originalDir.c_str()) == 0) {
            current = &slot;
            break;
        }
    }

    std::string err;
    std::wstring safetyBackup;
    if (current) {
        safetyBackup = backupSlot(*current, "before_restore", &err);
        if (safetyBackup.empty()) {
            app.saveNote = "还原已取消：无法先备份当前存档（" + err + "）";
            logLine(app.saveNote);
            return;
        }
    } else if (::GetFileAttributesW(info.originalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        app.saveNote = fmt("还原已取消：目标目录不是当前可识别的%s存档。", selectedGameDescriptor().shortName);
        logLine(app.saveNote);
        return;
    }
    if (!restoreBackup(info, &err)) {
        app.saveNote = "还原失败：" + err;
        logLine(app.saveNote);
        return;
    }
    app.saveNote = fmt("已还原 %s / %s 到 %s%s", W2U(info.profile).c_str(),
                       W2U(info.slot).c_str(), W2U(info.originalDir).c_str(),
                       safetyBackup.empty()
                           ? ""
                           : ("；还原前状态已备份到 " + W2U(safetyBackup)).c_str());
    logLine(app.saveNote);
    app.slots = listSlots();
    app.backups = listBackups();
    app.selectedBackup = -1;
}

}  // namespace

namespace {

bool isDirSegment(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool exportFilePathExists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring sanitizeExportPart(const std::wstring& raw) {
    std::wstring out;
    for (wchar_t ch : raw) {
        if (ch < 0x20 || ch == 0x7F) continue;
        if (wcschr(L"<>:\"/\\|?*", ch) != nullptr) continue;
        out.push_back(ch);
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    if (out.empty()) out = L"unknown";
    if (out == L"." || out == L"..") out = L"unknown";
    if (out.size() > 40) out.resize(40);
    return out;
}

std::wstring exportTimestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return buf;
}

}  // namespace

namespace {

void consumePendingExport(AppState& app) {
    PendingSaveExport result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingExport.ready) return;
        result = std::move(app.pendingExport);
        app.pendingExport = {};
    }
    if (result.game != selectedGame()) return;
    app.saveNote = std::move(result.note);
    logLine(app.saveNote);
}

void startSelectedExport(AppState& app, bool readable) {
    if (app.busy) {
        app.saveNote = "已有后台任务在进行，请等待完成或取消后再导出";
        return;
    }
    if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "请先在列表里选中一个存档";
        return;
    }
    joinWorker(app);
    consumePendingExport(app);
    const SaveSlot slot = app.slots[(size_t)app.selectedSlot];
    const GameId game = selectedGame();
    const std::wstring exports = documentsDir() + L"\\exports";
    if (!isDirSegment(exports) && !::CreateDirectoryW(exports.c_str(), nullptr)) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_ALREADY_EXISTS || !isDirSegment(exports)) {
            app.saveNote = "导出失败：无法创建导出目录（可能是权限不足或路径被占用）";
            logLine(app.saveNote);
            return;
        }
    }
    const std::wstring base = (readable ? L"readable_" : L"save_") + sanitizeExportPart(slot.profileName) + L"_" +
                              sanitizeExportPart(slot.slotName) + L"_" + exportTimestamp();
    std::wstring outPath;
    for (int i = 0; i < 1000; ++i) {
        std::wstring candidate = exports + L"\\" + base + (i == 0 ? L"" : L"_" + std::to_wstring(i)) + L".sii";
        if (!exportFilePathExists(candidate)) { outPath = candidate; break; }
    }
    if (outPath.empty()) {
        app.saveNote = "导出失败：同名文件过多，请清理导出目录后重试";
        logLine(app.saveNote);
        return;
    }
    app.cancel = false;
    app.progressDone = 0;
    app.progressTotal = 1;
    app.progressNote = readable ? "导出可读 SII" : "解密导出存档";
    app.saveNote = readable ? "正在后台解密并转换可读 SII 文本……" : "正在后台解密导出存档……";
    app.busy = true;
    try {
        app.worker = std::thread([&app, slot, outPath, game, readable]() {
            PendingSaveExport result;
            result.game = game;
            result.ready = true;
            try {
                const auto canceled = [&app] { return app.cancel.load(); };
                std::string err;
                if (readable) {
                    ReadableExportInfo info;
                    if (!exportReadableSii(slot, outPath, &info, &err, canceled)) {
                        result.note = "可读导出失败：" + (err.empty() ? "未知错误" : err);
                    } else {
                        result.note = fmt("已导出可读 SII（%s，%s）-> %s。重复导出不会覆盖旧文件",
                            info.innerFormat == "bsii" ? "BSII v3 已转换为文本" : "SII 文本原样保留",
                            formatSize(info.textSize).c_str(), W2U(outPath).c_str());
                    }
                } else {
                    DecryptInfo info;
                    if (!exportDecrypted(slot, outPath, &info, &err, canceled)) {
                        result.note = "解密导出失败：" + (err.empty() ? "未知错误" : err);
                    } else {
                        const char* kind = info.innerFormat == "text" ? "SII 文本" :
                                           info.innerFormat == "bsii" ? "BSII 二进制，保留原格式" :
                                           "3nK 二进制，保留原格式";
                        result.note = fmt("已解密导出（%s，%s）-> %s。重复导出不会覆盖旧文件",
                            kind, formatSize(info.innerSize).c_str(), W2U(outPath).c_str());
                    }
                }
            } catch (const std::exception& e) {
                result.note = std::string("导出失败：") + e.what();
            } catch (...) {
                result.note = "导出失败：发生未知异常";
            }
            {
                std::lock_guard<std::mutex> lock(app.resultMutex);
                app.pendingExport = std::move(result);
            }
            app.progressDone = 1;
            app.busy = false;
        });
    } catch (const std::exception& e) {
        app.busy = false;
        app.saveNote = std::string("无法启动导出任务：") + e.what();
        logLine(app.saveNote);
    }
}

}  // namespace

void exportSelectedSlot(AppState& app) { startSelectedExport(app, false); }
void exportReadableSelectedSlot(AppState& app) { startSelectedExport(app, true); }

void applyTextPatch(AppState& app) {
    // 存档编辑仅在游戏完全退出后允许：避免游戏自动存档覆盖新值、读写文件竞争，
    // 同时杜绝在线会话中通过存档编辑绕过进程内存写入闸门。
    DWORD runningPid = 0;
    if (findProcess(selectedProcessName(), &runningPid, nullptr)) {
        app.saveNote = "请先完全退出游戏，再修改明文存档（游戏运行中可能自动保存并覆盖该文件）。";
        logLine(app.saveNote);
        return;
    }
    if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "请先在列表里选中一个存档";
        return;
    }
    SaveSlot slot = app.slots[(size_t)app.selectedSlot];
    if (slot.format != "text") {
        app.saveNote = "只对「明文(SiiNunit)」存档有效；加密/二进制存档不写入。";
        return;
    }
    double money = 0, xp = 0;
    bool setMoney = parseNumber(app.saveMoney, false, &money);
    bool setXp = parseNumber(app.saveXp, false, &xp);
    if (!setMoney && !setXp) {
        app.saveNote = "请填写要写入的现金或经验";
        return;
    }
    std::string err;
    std::wstring backup = backupSlot(slot, "before_text_edit", &err);
    if (backup.empty()) {
        app.saveNote = "写入前备份失败：" + err;
        logLine(app.saveNote);
        return;
    }
    if (!patchTextSave(slot.gameSii, setMoney, (int64_t)money, setXp, (int64_t)xp, &err)) {
        app.saveNote = "写入失败：" + err;
        logLine(app.saveNote);
        return;
    }
    app.saveNote = fmt("已写入明文存档（备份：%s）。进游戏读取存档「%s」即可看到新数值。",
                       W2U(backup).c_str(), W2U(slot.slotName).c_str());
    logLine(app.saveNote);
    app.backups = listBackups();
    selectSlot(app, app.selectedSlot);
}

// ==========================================================================
// 界面渲染
// ==========================================================================
namespace {

void renderValuePanel(AppState& app, PanelKind kind, const char* title, const char* hint,
                      VType* ptype, char* current, char* target, bool* lockVar) {
    const char* const titleId = title;
    ::ImGui::PushID((int)kind);
    ::ImGui::BeginChild(titleId, ImVec2(0, 0),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_None);

    // Card Header Bar
    ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ %s", title);
    if (lockVar && *lockVar) {
        ::ImGui::SameLine();
        ::ImGui::TextColored(ImVec4(0.22f, 0.85f, 0.52f, 1.0f), "[ 锁定保持中 ]");
    }
    ::ImGui::Separator();
    ::ImGui::Spacing();

    // 步骤 1: 扫描定位
    ::ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f), "① 扫描");
    ::ImGui::SameLine(220.0f);
    ::ImGui::TextUnformatted("类型");
    ::ImGui::SameLine();
    typeCombo("##type", ptype);
    if (vtypeInfo(*ptype).isFloat) {
        ::ImGui::SameLine();
        ::ImGui::TextUnformatted("容差");
        ::ImGui::SameLine();
        ::ImGui::SetNextItemWidth(70);
        ::ImGui::InputText("##tol", toleranceFor(app, kind), 16, ImGuiInputTextFlags_CharsDecimal);
    }

    ::ImGui::TextUnformatted("游戏当前值");
    ::ImGui::SameLine(100.0f);
    ::ImGui::SetNextItemWidth(170);
    ::ImGui::InputTextWithHint("##current", "当前数值", current, 32, ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button(" 首次扫描 ", ImVec2(120, 26))) startFirstScan(app, kind);
    ::ImGui::SameLine();
    if (::ImGui::Button(" 变动后收窄 ", ImVec2(120, 26))) startNextScan(app, kind, ScanMode::Exact);
    ::ImGui::SameLine();
    if (::ImGui::Button(" 未变收窄 ", ImVec2(110, 26))) startNextScan(app, kind, ScanMode::Unchanged);
    ::ImGui::EndDisabled();

    ::ImGui::Spacing();

    // 步骤 2: 修改生效
    ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "② 写入 / 锁定");
    ::ImGui::TextUnformatted("目标修改值");
    ::ImGui::SameLine(100.0f);
    ::ImGui::SetNextItemWidth(170);
    ::ImGui::InputTextWithHint("##target", "目标数值", target, 32, ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button(" 写入修改(立即生效) ", ImVec2(170, 26))) writePanelValues(app, kind);
    ::ImGui::SameLine();
    if (::ImGui::Checkbox("锁定此数值", lockVar)) togglePanelLock(app, kind, *lockVar);
    ::ImGui::SameLine();
    if (::ImGui::Button("解除锁定", ImVec2(80, 26))) {
        *lockVar = false;
        togglePanelLock(app, kind, false);
    }
    ::ImGui::EndDisabled();

    // 快速预设按钮
    if (kind == PanelKind::Money) {
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.72f, 1.0f), "预设");
        ::ImGui::SameLine();
        if (::ImGui::Button("+100万")) {
            double cur = 0;
            parseNumber(target, false, &cur);
            if (cur <= 0) parseNumber(current, false, &cur);
            cur += 1000000.0;
            char buf[32];
            ::snprintf(buf, sizeof(buf), "%.0f", cur);
            ::strncpy_s(target, 32, buf, _TRUNCATE);
        }
        ::ImGui::SameLine();
        if (::ImGui::Button("1,000 万")) ::strncpy_s(target, 32, "10000000", _TRUNCATE);
        ::ImGui::SameLine();
        if (::ImGui::Button("5,000 万")) ::strncpy_s(target, 32, "50000000", _TRUNCATE);
        ::ImGui::SameLine();
        if (::ImGui::Button("1 亿")) ::strncpy_s(target, 32, "100000000", _TRUNCATE);
        ::ImGui::SameLine();
        if (::ImGui::Button("9.99 亿(上限)")) ::strncpy_s(target, 32, "999999999", _TRUNCATE);
    } else if (kind == PanelKind::Xp) {
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.72f, 1.0f), "预设");
        ::ImGui::SameLine();
        if (::ImGui::Button("5,000 (约10级)")) ::strncpy_s(target, 32, "5000", _TRUNCATE);
        ::ImGui::SameLine();
        if (::ImGui::Button("50,000 (约35级)")) ::strncpy_s(target, 32, "50000", _TRUNCATE);
        ::ImGui::SameLine();
        if (::ImGui::Button("150,000 (满级技能)")) ::strncpy_s(target, 32, "150000", _TRUNCATE);
        ::ImGui::SameLine();
        if (::ImGui::Button("1,000,000 (百万经验)")) ::strncpy_s(target, 32, "1000000", _TRUNCATE);
    }

    ::ImGui::Spacing();
    const std::string& st = statusFor(app, kind);
    if (!st.empty()) {
        ::ImGui::TextColored(ImVec4(0.40f, 0.78f, 0.95f, 1.0f), "状态：%s", st.c_str());
    } else {
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "提示：%s", hint);
    }
    ::ImGui::EndChild();
    ::ImGui::PopID();
}

// ==========================================================================
// 金钱 / 经验：结构化定位（不依赖手动收窄）
// ==========================================================================
namespace {

// 结构校验时最多考察的候选数量（与旧版一致的上限）
constexpr size_t kEconomyProbeCap = 200000;

}  // namespace

// ---------------------------------------------------------------------------
// 工作线程结果 -> 界面状态。只在界面线程调用；锁内仅做拷贝/清空，不阻塞渲染。
// ---------------------------------------------------------------------------
namespace {

void consumePendingEconomy(AppState& app) {
    PendingEconomyResult result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingEconomy.ready) return;
        result = std::move(app.pendingEconomy);
        app.pendingEconomy = PendingEconomyResult{};
    }
    if (!result.log.empty()) logLine(result.log);
    if (result.cancelled) {
        app.locateStatus = "结构化定位已取消。";
        return;
    }
    // 结构化数据统一在这里落地（工作线程不再直接写 located* / locateStatus）
    app.locatedMoneyAddress = result.moneyLocated ? result.moneyAddress : 0;
    app.locatedMoneyBank = result.moneyLocated ? result.moneyBank : 0;
    app.locatedMoneyValue = result.moneyLocated ? result.moneyValue : 0;
    app.locatedMoneyDetail = result.moneyLocated ? result.moneyDetail : std::string();
    app.locatedXpAddress = result.xpLocated ? result.xpAddress : 0;
    app.locatedEconomy = result.xpLocated ? result.economyObject : 0;
    app.locatedXpValue = result.xpLocated ? result.xpValue : 0;
    app.locatedXpDetail = result.xpLocated ? result.xpDetail : std::string();

    // 组装状态文案
    std::string status;
    if (result.moneyTruncated) {
        status = "定位失败（已安全关闭）：" + result.moneyDetail + "。";
    } else if (result.moneyAmbiguous) {
        status = "定位失败（已安全关闭）：" + result.moneyDetail +
                 "。存在多个同分 bank 候选，无法唯一确定，已拒绝自动写入；"
                 "请让金额变化后重新定位，或用「高级：手动扫描」人工筛选。";
    } else if (result.moneyLocated) {
        status = fmt("已定位：金钱 %s", formatInt(result.moneyValue).c_str());
        if (result.xpTruncated) {
            status += fmt("（经验候选过多未完整检查：%s）", result.xpDetail.c_str());
        } else if (result.xpAmbiguous) {
            status += fmt("（经验定位存在歧义：%s，已拒绝自动写入经验）",
                          result.xpDetail.c_str());
        } else if (result.xpLocated) {
            status += fmt("，经验 %s", formatInt(result.xpValue).c_str());
        } else {
            status += "（经验未定位）";
        }
    } else {
        status = "未定位：" + result.moneyDetail;
        if (result.xpTruncated) status += "；经验：" + result.xpDetail;
    }

    if (result.autoWriteMoney) {
        if (result.moneyWriteOk) {
            app.locatedMoneyValue = result.moneyWritten;
            status += fmt("。已直接写入金钱 = %s（回读一致），无需锁定。",
                          formatInt(result.moneyWritten).c_str());
        } else {
            status += "。金钱未写入：" +
                      (result.moneyWriteError.empty()
                           ? std::string("目标金额无效")
                           : result.moneyWriteError);
        }
    }
    if (result.autoWriteXp) {
        if (result.xpWriteOk) {
            app.locatedXpValue = result.xpWritten;
            status += fmt("。已直接写入经验 = %s（回读一致），无需锁定。",
                          formatInt(result.xpWritten).c_str());
        } else {
            status += "。经验未写入：" +
                      (result.xpWriteError.empty()
                           ? std::string("目标经验必须是 32 位整数")
                           : result.xpWriteError);
        }
    }
    app.locateStatus = std::move(status);
    logLine(app.locateStatus);
}

}  // namespace

}  // namespace

// ---------------------------------------------------------------------------
// 经济定位 / 直接写入（供界面与离线测试共用；声明见 ui.h）
// ---------------------------------------------------------------------------
void clearLocatedEconomy(AppState& app) {
    // 清除定位结果时必须一并解除锁定，否则后台保持线程会继续写这两个地址
    if (app.freezer) {
        if (app.locatedMoneyAddress) app.freezer->remove(app.locatedMoneyAddress);
        if (app.locatedXpAddress) app.freezer->remove(app.locatedXpAddress);
    }
    app.locatedMoneyAddress = 0;
    app.locatedMoneyBank = 0;
    app.locatedMoneyValue = 0;
    app.locatedMoneyDetail.clear();
    app.locatedXpAddress = 0;
    app.locatedEconomy = 0;
    app.locatedXpValue = 0;
    app.locatedXpDetail.clear();
    app.locateStatus = "已清除定位结果。";
}

void startLocateEconomy(AppState& app) {
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = false;
        app.locateStatus = reason;
        logLine(reason);
        return;
    }
    if (app.busy.load()) {
        app.locateStatus = "已有任务在进行，请等它结束。";
        return;
    }
    if (!ensureAttached(app)) {
        app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = false;
        app.locateStatus = "还没有附加到游戏进程。";
        return;
    }
    const bool atsAuto = selectedGame() == GameId::Ats;
    double moneyValue = 0.0;
    if (atsAuto) {
        uint64_t imageBase = 0;
        if (!verifyGameLayout(app.mem, &imageBase, &app.locateStatus)) {
            app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = false;
            logLine(app.locateStatus);
            return;
        }
        moneyValue = static_cast<double>(imageBase + layoutProfile(GameId::Ats).bankVtableRva);
    } else if (!parseNumber(app.moneyCurrent, false, &moneyValue)) {
        app.locateStatus = "请先在「游戏当前值」里填写游戏里显示的当前金钱。";
        return;
    }
    double xpValue = 0.0;
    const bool hasXp = parseNumber(app.xpCurrent, false, &xpValue);

    joinWorker(app);
    applyPending(app);
    consumePendingEconomy(app);  // 先消费上一轮未显示的结果，防止覆盖
    const int workers = app.workers;
    const size_t regionCap = maxRegionBytes(app);
    // 「直接改钱/直接改经验」的一键流程：把自动写入意图拍成快照带进工作线程。
    // 捕获后立刻清掉界面侧标志，避免上一轮的意图残留到下一次手动定位。
    const bool autoWriteMoney = app.autoWriteMoneyAfterLocate;
    const bool autoWriteXp = app.autoWriteXpAfterLocate;
    app.autoWriteMoneyAfterLocate = false;
    app.autoWriteXpAfterLocate = false;
    // 输入快照：工作线程绝不读取界面可编辑的字符缓冲区（moneyTarget/xpTarget 等），
    // 全部输入参数在启动线程前于界面线程复制成值；越界/非法值在此就判定并带进线程，
    // 绝不在工作线程里做越界转换。
    int64_t moneyTargetValue = 0;
    std::string moneyTargetError;
    const bool moneyTargetValid = parseMoneyTarget(app.moneyTarget, &moneyTargetValue,
                                                   &moneyTargetError);
    int32_t xpTargetValue = 0;
    std::string xpTargetError;
    const bool xpTargetValid = parseXpTarget(app.xpTarget, &xpTargetValue, &xpTargetError);
    // 写入闸门快照在真正写入前重新评估（见 worker 内的 gateAtWrite）
    app.busy.store(true);
    app.cancel.store(false);
    app.progressDone.store(0);
    app.progressTotal.store(2);
    app.progressNote = "结构化定位：查找金额";
    app.locateStatus = atsAuto ? "定位中：按美卡银行类型寻找当前金额对象…" : "定位中：在内存里寻找与当前金额一致的 bank 对象…";
    logLine("开始结构化定位金钱/经验（按当前游戏独立运行时布局）");

    app.worker = std::thread([&app, moneyValue, atsAuto, xpValue, hasXp, workers, regionCap,
                              autoWriteMoney, autoWriteXp, moneyTargetValue, moneyTargetValid,
                              moneyTargetError, xpTargetValue, xpTargetValid,
                              xpTargetError]() {
        // 线程安全约定：本线程只触碰 app 的原子量（progressDone/progressTotal/cancel/
        // busy）与 resultMutex 保护的 pending 通道；对游戏内存只做定位线程内的读写。
        ScanOptions opt;
        opt.workers = workers;
        opt.maxRegionSize = regionCap;
        auto progressFn = [&app](uint64_t done, uint64_t total) {
            app.progressDone.store(done);
            app.progressTotal.store(total ? total : 1);
        };
        auto cancelFn = [&app]() { return app.cancel.load(); };

        PendingEconomyResult result;

        // 1) 按金额找 bank 对象（最高分候选唯一且候选完整才可用；歧义/截断一律失败关闭）
        ScanSession moneyScan(app.mem, VType::Int64, 8, 0.0);
        const uint64_t moneyHits =
            moneyScan.firstScan(moneyValue, opt, progressFn, cancelFn);
        if (!app.cancel.load() && moneyHits > 0) {
            std::vector<uint64_t> moneyFields = moneyScan.addresses();
            if (atsAuto) for (auto& address : moneyFields) address += ats_economy_offsets::kMoney;
            const CandidatePick pick = pickBestBankCandidate(app.mem, moneyFields, kEconomyProbeCap);
            if (pick.truncated) {
                result.moneyTruncated = true;
                result.moneyDetail =
                    fmt("金额候选 %llu 个，超过本次可检查上限 %llu 个（只检查了前 %llu 个）："
                        "无法保证不存在同分或更高分的 bank 对象，已拒绝写入；"
                        "请缩小扫描范围（降低「跳过超大内存区」阈值）或继续收窄候选后重试",
                        (unsigned long long)moneyHits, (unsigned long long)kEconomyProbeCap,
                        (unsigned long long)pick.examined);
            } else if (pick.unique) {
                result.moneyLocated = true;
                result.moneyAddress = pick.fieldAddress;
                result.moneyBank = pick.object;
                result.moneyValue = pick.value;
                result.moneyDetail = fmt("金额候选 %llu 个，唯一可信 bank 对象（校验 %d 分）",
                                         (unsigned long long)moneyHits, pick.score);
            } else if (pick.probed) {
                result.moneyAmbiguous = true;
                result.moneyDetail =
                    fmt("金额候选 %llu 个，结构校验通过且同分最高分候选 %d 个",
                        (unsigned long long)moneyHits, pick.tied);
            } else {
                result.moneyDetail = fmt("金额候选 %llu 个，但都没有通过 bank 结构校验",
                                         (unsigned long long)moneyHits);
                if (moneyFields.size() == 1) {
                    ProbeResult rejected;
                    if (!probeBankObject(app.mem, moneyFields.front(), &rejected) && !rejected.detail.empty())
                        result.moneyDetail += "：" + rejected.detail;
                }
            }
        } else if (!app.cancel.load()) {
            result.moneyDetail = atsAuto ? "未找到美卡银行类型对象，请确认存档已加载后重新定位" : "内存中没有找到与当前金额一致的数值（可在游戏里买卖一次让金额变化后重试）";
        }

        app.progressDone.store(1);
        // 阶段性进度说明走 pending 通道（旧实现直接写 app.progressNote，属数据竞争）
        postResult(app, PanelKind::Money, std::string(), std::string(),
                   "结构化定位：由 bank 指针反查经济对象");

        // 2) 由「指向 bank 的指针」反查经济对象（经验地址随之确定）。
        //    候选必须真的引用已定位的 bank，且唯一同分才可用。
        if (result.moneyLocated && !app.cancel.load()) {
            ScanSession bankRefScan(app.mem, VType::Int64, 8, 0.0);
            const uint64_t refHits =
                bankRefScan.firstScan((double)result.moneyBank, opt, progressFn, cancelFn);
            if (!app.cancel.load() && refHits > 0) {
                const CandidatePick pick = pickBestEconomyFromBankRefs(
                    app.mem, bankRefScan.addresses(), result.moneyBank, kEconomyProbeCap);
                if (pick.truncated) {
                    result.xpTruncated = true;
                    result.xpDetail =
                        fmt("bank 引用 %llu 处，超过本次可检查上限 %llu 个（只检查了前 %llu 个）："
                            "无法保证不存在同分或更高分的经济对象，已拒绝写入经验",
                            (unsigned long long)refHits, (unsigned long long)kEconomyProbeCap,
                            (unsigned long long)pick.examined);
                } else if (pick.unique) {
                    result.xpLocated = true;
                    result.xpAddress = pick.fieldAddress;
                    result.economyObject = pick.object;
                    result.xpValue = pick.value;
                    result.xpDetail =
                        fmt("由 %llu 处 bank 引用确认经济对象（校验 %d 分，引用一致）",
                            (unsigned long long)refHits, pick.score);
                } else if (pick.probed) {
                    result.xpAmbiguous = true;
                    result.xpDetail =
                        fmt("bank 引用 %llu 处，同分经济对象候选 %d 个",
                            (unsigned long long)refHits, pick.tied);
                } else {
                    result.xpDetail = "找到 bank 引用，但经济对象结构校验未通过";
                }
            } else if (!app.cancel.load()) {
                result.xpDetail = "没有找到指向 bank 的引用（经济对象可能尚未加载）";
            }
        }

        // 3) 若仍没有经验地址，且用户填了经验值，则按经验值再定位一次。
        //    xpScan.addresses()[i] 本身就是经验字段地址（economy + 0x780），
        //    直接传给 probe —— 这里不能再加 kEconomyExperiencePoints（历史缺陷：
        //    会让对象基址错位）。已定位银行对象时还必须验证经济对象引用它。
        if (!result.xpLocated && hasXp && !app.cancel.load()) {
            ScanSession xpScan(app.mem, VType::Int32, 4, 0.0);
            const uint64_t xpHits = xpScan.firstScan(xpValue, opt, progressFn, cancelFn);
            if (!app.cancel.load() && xpHits > 0) {
                const uint64_t requiredBank = result.moneyLocated ? result.moneyBank : 0;
                const CandidatePick pick = pickBestEconomyFromXpFields(
                    app.mem, xpScan.addresses(), requiredBank, kEconomyProbeCap);
                if (pick.truncated) {
                    result.xpTruncated = true;
                    result.xpDetail =
                        fmt("经验值候选 %llu 个，超过本次可检查上限 %llu 个（只检查了前 %llu 个）："
                            "无法保证不存在同分或更高分的经济对象，已拒绝写入经验",
                            (unsigned long long)xpHits, (unsigned long long)kEconomyProbeCap,
                            (unsigned long long)pick.examined);
                } else if (pick.unique) {
                    result.xpLocated = true;
                    result.xpAddress = pick.fieldAddress;
                    result.economyObject = pick.object;
                    result.xpValue = pick.value;
                    result.xpDetail =
                        fmt("按经验值定位：候选 %llu 个，唯一可信经济对象（校验 %d 分%s）",
                            (unsigned long long)xpHits, pick.score,
                            requiredBank ? "，已确认引用已定位的 bank 对象" : "");
                } else if (pick.probed) {
                    result.xpAmbiguous = true;
                    result.xpDetail =
                        fmt("按经验值定位：候选 %llu 个，同分经济对象候选 %d 个",
                            (unsigned long long)xpHits, pick.tied);
                } else {
                    result.xpDetail =
                        fmt("经验值候选 %llu 个，但结构校验未通过%s",
                            (unsigned long long)xpHits,
                            requiredBank ? "（或未引用已定位的 bank 对象）" : "");
                }
            } else if (!app.cancel.load()) {
                result.xpDetail = "内存中没有找到与当前经验一致的数值";
            }
        }

        // 4) 「直接改钱 / 直接改经验」：用启动前复制的目标值快照做验证写入。
        // 4) 「直接改钱 / 直接改经验」：用启动前复制的目标值快照做验证写入。
        //    扫描可能持续数十秒，因此在这里（真正写入前一刻）重新评估写入闸门。
        const std::string gateAtWrite = app.cancel.load() ? std::string() : writeGateReason(app);
        if (!app.cancel.load() && autoWriteMoney && result.moneyLocated) {
            result.autoWriteMoney = true;
            // 扫描可能持续数十秒：真正写入前用最新状态复核写入闸门
            if (!gateAtWrite.empty()) {
                result.moneyWriteError = "联机保护：" + gateAtWrite + "（未写入）";
            } else if (moneyTargetValid) {
                std::string error;
                if (writeBankMoneyGuarded(app, result.moneyAddress, result.moneyBank,
                                          (int64_t)moneyTargetValue, &error)) {
                    result.moneyWriteOk = true;
                    result.moneyWritten = (int64_t)moneyTargetValue;
                } else {
                    result.moneyWriteError = error;
                }
            } else {
                result.moneyWriteError = moneyTargetError.empty()
                                             ? std::string("「要改成的金额」不是有效数字")
                                             : moneyTargetError;
            }
        }
        if (!app.cancel.load() && autoWriteXp && result.xpLocated) {
            result.autoWriteXp = true;
            if (!gateAtWrite.empty()) {
                result.xpWriteError = "联机保护：" + gateAtWrite + "（未写入）";
            } else if (xpTargetValid) {
                std::string error;
                if (writeXpGuarded(app, result.xpAddress, result.economyObject,
                                   (int32_t)xpTargetValue, &error)) {
                    result.xpWriteOk = true;
                    result.xpWritten = (int32_t)xpTargetValue;
                } else {
                    result.xpWriteError = error;
                }
            } else {
                result.xpWriteError = xpTargetError.empty()
                                          ? std::string("「要改成的经验」不是有效的 32 位整数")
                                          : xpTargetError;
            }
        }

        result.cancelled = app.cancel.load();
        result.log = "结构化定位：金钱 " +
                     (result.moneyLocated
                          ? fmt("0x%llX", (unsigned long long)result.moneyAddress)
                          : std::string("未定位")) +
                     "；经验 " +
                     (result.xpLocated ? fmt("0x%llX", (unsigned long long)result.xpAddress)
                                       : std::string("未定位"));
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            result.ready = true;
            app.pendingEconomy = std::move(result);
        }
        app.progressDone.store(app.progressTotal.load());
        app.busy.store(false);
    });
}

// ---------------------------------------------------------------------------
// 「直接改钱 / 直接改经验」一键入口：已定位 → 立即写入；未定位 → 自动先定位，
// 定位成功后由工作线程自动写入。全程免锁定。（声明见 ui.h）
// ---------------------------------------------------------------------------

void directModifyMoney(AppState& app) {
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        app.locateStatus = reason;
        logLine(reason);
        return;
    }
    if (app.busy.load()) {
        app.locateStatus = "已有任务在进行，请等它结束。";
        return;
    }
    int64_t target = 0;
    std::string targetError;
    if (!parseMoneyTarget(app.moneyTarget, &target, &targetError)) {
        app.locateStatus = "请先在「要改成的金额」里填写有效金额：" + targetError;
        return;
    }
    const std::string gate = writeGateReason(app);
    if (!gate.empty()) {
        app.locateStatus = "已拒绝写入：" + gate;
        logLine(app.locateStatus);
        return;
    }
    if (app.locatedMoneyAddress) {
        writeLocatedMoney(app, false);
        return;
    }
    double current = 0.0;
    if (selectedGame() == GameId::Ets2 && !parseNumber(app.moneyCurrent, false, &current)) {
        app.locateStatus = "第一次使用需要引导定位：把游戏里显示的当前金额填到「游戏当前值」，"
                           "再点「直接改钱」（之后就不需要再填了）。";
        return;
    }
    app.autoWriteMoneyAfterLocate = true;
    app.locateStatus = "尚未定位：先自动定位（约 10~25 秒），成功后立即直接写入目标金额。";
    startLocateEconomy(app);
}

void directModifyXp(AppState& app) {
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        app.locateStatus = reason;
        logLine(reason);
        return;
    }
    if (app.busy.load()) {
        app.locateStatus = "已有任务在进行，请等它结束。";
        return;
    }
    int32_t target = 0;
    std::string targetError;
    if (!parseXpTarget(app.xpTarget, &target, &targetError)) {
        app.locateStatus = "请先在「要改成的经验」里填写有效经验：" + targetError;
        return;
    }
    const std::string gate = writeGateReason(app);
    if (!gate.empty()) {
        app.locateStatus = "已拒绝写入：" + gate;
        logLine(app.locateStatus);
        return;
    }
    if (app.locatedXpAddress) {
        writeLocatedXp(app, false);
        return;
    }
    double current = 0.0;
    if (selectedGame() == GameId::Ets2 && !parseNumber(app.moneyCurrent, false, &current)) {
        app.locateStatus = "经验地址随金钱定位一并取得：把游戏里显示的当前金额填到「游戏当前值」，"
                           "再点「直接改经验」。";
        return;
    }
    app.autoWriteXpAfterLocate = true;
    app.locateStatus = "尚未定位：先自动定位（约 10~25 秒），成功后立即直接写入目标经验。";
    startLocateEconomy(app);
}

// 写入 / 锁定定位到的地址。
// 直接写入走「结构校验 + 范围检查 + 回读比对」的验证路径（writeBankMoneyVerified），
// 写的是游戏自己的权威存储：UI 显示、收支结算、自动存档都读这个字段，
// 因此一次性写入即可持久，**不需要后台锁定**。
void writeLocatedMoney(AppState& app, bool lock) {
    if (!app.locatedMoneyAddress) {
        app.locateStatus = "还没有定位结果：请填好当前金额后点「直接改钱」。";
        return;
    }
    int64_t money = 0;
    std::string targetError;
    if (!parseMoneyTarget(app.moneyTarget, &money, &targetError)) {
        // 目标数值越界/非法：不得清除定位结果，也不能写入
        app.locateStatus = "未写入：" + targetError + "（定位结果保留，可修改后重试）。";
        logLine(app.locateStatus);
        return;
    }
    // 真正执行写入前复核写入闸门（扫描/等待期间可能进入联机）
    if (lock) {
        ProbeResult probe;
        if (!probeBankObject(app.mem, app.locatedMoneyAddress, &probe) ||
            probe.object != app.locatedMoneyBank) {
            clearLocatedEconomy(app);
            app.locateStatus = "定位地址已失效（可能换过存档或重载），已清除定位结果，请重新定位。";
            return;
        }
        if (!app.freezer) {
            app.locateStatus = "锁定器尚未就绪（请先附加游戏）。";
            return;
        }
        const std::string gate = writeGateReason(app);
        if (!gate.empty()) {
            app.locateStatus = "已拒绝写入：" + gate + "（定位结果保留）";
            logLine(app.locateStatus);
            return;
        }
        app.freezer->addInt(app.locatedMoneyAddress, VType::Int64, money);
        app.locateStatus = fmt("已锁定金钱 = %s（可选保险：防止游戏内收支改变金额；"
                               "一般情况用「直接改钱」即可，无需锁定）",
                               formatInt(money).c_str());
    } else {
        std::string error;
        if (!writeBankMoneyGuarded(app, app.locatedMoneyAddress, app.locatedMoneyBank, money,
                                   &error)) {
            if (error.find("联机保护") != std::string::npos) {
                // 闸门拦截：不是地址失效，定位结果必须保留
                app.locateStatus = "已拒绝写入：" + error + "（定位结果保留）";
            } else if (error.find("结构校验未通过") != std::string::npos) {
                clearLocatedEconomy(app);
                app.locateStatus = "直接写入失败：" + error + "（定位结果已清除，请重新定位）";
            } else {
                app.locateStatus = "直接写入失败：" + error + "（定位结果保留，可重试）";
            }
            logLine(app.locateStatus);
            return;
        }
        app.locatedMoneyValue = money;
        app.locateStatus = fmt("已直接写入金钱 = %s（回读一致）。无需锁定：游戏读的就是这个"
                               "地址，自动存档会把它写进存档。",
                               formatInt(money).c_str());
    }
    logLine(app.locateStatus);
}

void writeLocatedXp(AppState& app, bool lock) {
    if (!app.locatedXpAddress) {
        app.locateStatus = "经验地址尚未定位（填好「游戏当前值」里的当前金额后点「直接改钱」，"
                           "定位金钱时会一并定位经验）。";
        return;
    }
    int32_t xp = 0;
    std::string targetError;
    if (!parseXpTarget(app.xpTarget, &xp, &targetError)) {
        // 越界/非整数绝不先转换：定位结果保留
        app.locateStatus = "未写入：" + targetError + "（定位结果保留，可修改后重试）。";
        logLine(app.locateStatus);
        return;
    }
    const std::string gate = writeGateReason(app);
    if (lock) {
        ProbeResult probe;
        if (!probeEconomyObject(app.mem, app.locatedXpAddress, &probe) ||
            probe.object != app.locatedEconomy) {
            clearLocatedEconomy(app);
            app.locateStatus = "经验地址已失效，已清除定位结果，请重新定位。";
            return;
        }
        if (!app.freezer) {
            app.locateStatus = "锁定器尚未就绪（请先附加游戏）。";
            return;
        }
        if (!gate.empty()) {
            app.locateStatus = "已拒绝写入：" + gate + "（定位结果保留）";
            logLine(app.locateStatus);
            return;
        }
        app.freezer->addInt(app.locatedXpAddress, VType::Int32, xp);
        app.locateStatus = fmt("已锁定经验 = %s（可选保险；一般情况用「直接改经验」即可，无需锁定）",
                               formatInt(xp).c_str());
    } else {
        std::string error;
        if (!writeXpGuarded(app, app.locatedXpAddress, app.locatedEconomy, xp, &error)) {
            if (error.find("联机保护") != std::string::npos) {
                app.locateStatus = "已拒绝写入：" + error + "（定位结果保留）";
            } else if (error.find("结构校验未通过") != std::string::npos) {
                clearLocatedEconomy(app);
                app.locateStatus = "直接写入失败：" + error + "（定位结果已清除，请重新定位）";
            } else {
                app.locateStatus = "直接写入失败：" + error + "（定位结果保留，可重试）";
            }
            logLine(app.locateStatus);
            return;
        }
        app.locatedXpValue = xp;
        app.locateStatus = fmt("已直接写入经验 = %s（回读一致）。无需锁定：升级与技能点会立刻结算，"
                               "自动存档会把它写进存档。",
                               formatInt(xp).c_str());
    }
    logLine(app.locateStatus);
}

void renderMoneyTab(AppState& app) {
    if (!supportsVerifiedStructures(selectedGame())) {
        ::ImGui::TextWrapped("美卡自动金钱/经验定位尚未适配。请使用下面的通用扫描，或在存档管理中修改支持的明文字段。");
        renderValuePanel(app, PanelKind::Money, "现金（手动扫描）", "输入当前金额，变化后收窄。",
                         &app.moneyType, app.moneyCurrent, app.moneyTarget, &app.moneyLock);
        renderValuePanel(app, PanelKind::Xp, "经验（手动扫描）", "输入当前经验，变化后收窄。",
                         &app.xpType, app.xpCurrent, app.xpTarget, &app.xpLock);
        return;
    }
    // ---------------- 主流程：直接修改（免锁定） ----------------
    ::ImGui::BeginChild("##locate_economy", ImVec2(0, 0),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ 直接修改（推荐 · 写完即生效，无需锁定）");
    ::ImGui::Separator();
    ::ImGui::Spacing();
    ::ImGui::TextWrapped("程序定位游戏自己的权威存储（bank.money_account / economy.experience_points）"
                         "后一次性写入。美卡自动识别银行对象，无需填写当前金额。游戏界面、收支结算、自动存档读的都是这两个字段，"
                         "所以写完不会被改回去，也不需要「锁定」。");
    ::ImGui::Spacing();

    // 第一次使用的引导输入：定位需要用当前金额做一次种子扫描。
    // 定位期间禁用输入：工作线程只使用启动前复制的快照，界面缓冲区不能同时被编辑。
    const bool inputsLocked = app.busy.load();
    if (!app.locatedMoneyAddress && selectedGame() == GameId::Ets2) {
        ::ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f), "① 当前金额（仅首次定位需要）");
        ::ImGui::SameLine(220.0f);
        ::ImGui::SetNextItemWidth(170);
        ::ImGui::BeginDisabled(inputsLocked);
        ::ImGui::InputTextWithHint("##current", "游戏里显示的金额", app.moneyCurrent, 32,
                                   ImGuiInputTextFlags_CharsDecimal);
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "定位用它做种子，约 10~25 秒");
    }

    ::ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                         app.locatedMoneyAddress ? "② 金额" : "② 金额（定位后可反复直接改）");
    ::ImGui::SameLine(220.0f);
    ::ImGui::SetNextItemWidth(170);
    ::ImGui::BeginDisabled(inputsLocked);
    ::ImGui::InputTextWithHint("##target_money", "目标金额", app.moneyTarget, 32,
                               ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button(" 直接改钱 ", ImVec2(120, 28))) directModifyMoney(app);
    ::ImGui::EndDisabled();

    // 快速预设（同样会改输入缓冲区，定位期间一并禁用）
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(inputsLocked);
    if (::ImGui::Button("+100万")) {
        double cur = 0;
        parseNumber(app.moneyTarget, false, &cur);
        if (cur <= 0) parseNumber(app.moneyCurrent, false, &cur);
        cur += 1000000.0;
        char buf[32];
        ::snprintf(buf, sizeof(buf), "%.0f", cur);
        ::strncpy_s(app.moneyTarget, 32, buf, _TRUNCATE);
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("1,000 万")) ::strncpy_s(app.moneyTarget, 32, "10000000", _TRUNCATE);
    ::ImGui::SameLine();
    if (::ImGui::Button("5,000 万")) ::strncpy_s(app.moneyTarget, 32, "50000000", _TRUNCATE);
    ::ImGui::SameLine();
    if (::ImGui::Button("1 亿")) ::strncpy_s(app.moneyTarget, 32, "100000000", _TRUNCATE);
    ::ImGui::SameLine();
    if (::ImGui::Button("9.99 亿")) ::strncpy_s(app.moneyTarget, 32, "999999999", _TRUNCATE);
    ::ImGui::EndDisabled();

    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                         app.locatedXpAddress ? "③ 经验" : "③ 经验（随金额一并定位）");
    ::ImGui::SameLine(220.0f);
    ::ImGui::SetNextItemWidth(170);
    ::ImGui::BeginDisabled(inputsLocked);
    ::ImGui::InputTextWithHint("##target_xp", "目标经验", app.xpTarget, 32,
                               ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button(" 直接改经验 ", ImVec2(130, 28))) directModifyXp(app);
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(inputsLocked);
    if (::ImGui::Button("5,000 (约10级)")) ::strncpy_s(app.xpTarget, 32, "5000", _TRUNCATE);
    ::ImGui::SameLine();
    if (::ImGui::Button("50,000 (约35级)")) ::strncpy_s(app.xpTarget, 32, "50000", _TRUNCATE);
    ::ImGui::SameLine();
    if (::ImGui::Button("150,000 (满级技能)")) ::strncpy_s(app.xpTarget, 32, "150000", _TRUNCATE);
    ::ImGui::SameLine();
    if (::ImGui::Button("1,000,000")) ::strncpy_s(app.xpTarget, 32, "1000000", _TRUNCATE);
    ::ImGui::EndDisabled();

    ::ImGui::Spacing();
    ::ImGui::Separator();
    ::ImGui::Spacing();

    const bool hasMoney = app.locatedMoneyAddress != 0;
    const bool hasXp = app.locatedXpAddress != 0;
    if (hasMoney) {
        ::ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.55f, 1.0f),
                             "金钱 0x%llX = %s", (unsigned long long)app.locatedMoneyAddress,
                             formatInt(app.locatedMoneyValue).c_str());
        if (!app.locatedMoneyDetail.empty()) {
            ::ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.75f, 1.0f), "%s",
                                 app.locatedMoneyDetail.c_str());
        }
    }
    if (hasXp) {
        ::ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.55f, 1.0f),
                             "经验 0x%llX = %s", (unsigned long long)app.locatedXpAddress,
                             formatInt(app.locatedXpValue).c_str());
        if (!app.locatedXpDetail.empty()) {
            ::ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.75f, 1.0f), "%s",
                                 app.locatedXpDetail.c_str());
        }
    }
    ::ImGui::TextColored(ImVec4(0.40f, 0.78f, 0.95f, 1.0f), "%s", app.locateStatus.c_str());

    // 可选保险：锁定（一般不需要）。直接写权威地址后游戏不会改回去；
    // 只有想「钉死」数值不受游戏内收支影响时才用。
    if (ImGui::CollapsingHeader("可选：锁定保险（一般不需要）")) {
        ::ImGui::BeginDisabled(app.busy.load() || !hasMoney);
        if (::ImGui::Button(" 锁定金钱 ", ImVec2(110, 0))) writeLocatedMoney(app, true);
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        ::ImGui::BeginDisabled(app.busy.load() || !hasXp);
        if (::ImGui::Button(" 锁定经验 ", ImVec2(110, 0))) writeLocatedXp(app, true);
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        if (::ImGui::Button(" 解除锁定 ", ImVec2(100, 0))) {
            if (app.freezer) {
                if (app.locatedMoneyAddress) app.freezer->remove(app.locatedMoneyAddress);
                if (app.locatedXpAddress) app.freezer->remove(app.locatedXpAddress);
            }
            app.locateStatus = "已解除金钱/经验锁定（直接写入的数值不受影响）。";
        }
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f),
                             "锁定 = 每 0.25 秒把数值写回一次，用来抵消游戏内真实的收支变化。"
                             "只是可选项，直接修改本身不需要它。");
        if (app.freezer && app.freezer->blocked() > 0) {
            ::ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                                 "锁定写入已被联机闸门拦截 %llu 次：%s（解锁条目仍保留，"
                                 "退出联机后自动恢复写入）",
                                 (unsigned long long)app.freezer->blocked(),
                                 app.freezer->blockedReason().c_str());
        }
    }
    ::ImGui::EndChild();

    ::ImGui::Spacing();

    // ---------------- 兼容保留：重新定位 / 清除 ----------------
    if (ImGui::CollapsingHeader("维护：重新定位 / 清除（换存档或读档后用）")) {
        ::ImGui::BeginDisabled(app.busy.load());
        if (::ImGui::Button(" 仅重新定位（不写入） ", ImVec2(170, 26))) startLocateEconomy(app);
        ::ImGui::SameLine();
        if (::ImGui::Button(" 清除定位结果 ", ImVec2(140, 26))) clearLocatedEconomy(app);
        ::ImGui::EndDisabled();
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f),
                             "定位地址绑定当前进程：换存档、读档或重启游戏后，直接写入前会自动"
                             "重新校验结构，失效时会提示重新定位。");
    }

    ::ImGui::Spacing();

    // ---------------- 高级：手动扫描（兼容性排查用） ----------------
    if (ImGui::CollapsingHeader("高级：手动扫描收窄（兼容性排查用，一般不需要）")) {
        ::ImGui::TextWrapped("Cheat Engine 式全内存扫描：只在直接修改定位失败时用于排查。"
                             "注意扫描命中可能包含镜像副本，写错副本会被游戏改回去（这正是"
                             "老版本需要「锁定」的原因）。");
        ::ImGui::Spacing();
        renderValuePanel(app, PanelKind::Money, "现金（钱）",
                         "填游戏里的现金 → 首次扫描；金额变化后填新值 → 变动后收窄。",
                         &app.moneyType, app.moneyCurrent, app.moneyTarget, &app.moneyLock);
        ::ImGui::Spacing();
        renderValuePanel(app, PanelKind::Xp, "经验值",
                         "32 位整数；改动会连带升级并给技能点。",
                         &app.xpType, app.xpCurrent, app.xpTarget, &app.xpLock);
    }
}

void renderVehicleTabImpl(AppState& app) {
    if (!supportsVerifiedStructures(selectedGame())) {
        ::ImGui::TextWrapped("美卡车辆固定内存功能尚未适配；当前仅显示只读遥测。手动扫描不会使用欧卡偏移。");
        if (::GetTickCount64() - app.telemetryCheckedAt >= 1000) {
            app.telemetryCheckedAt = ::GetTickCount64();
            app.telemetryAvailable = readTelemetry(&app.telemetry, &app.telemetryError);
        }
        if (app.telemetryAvailable) {
            ::ImGui::Text("油量：%.2f / %.2f L", app.telemetry.fuel, app.telemetry.fuelCapacity);
            ::ImGui::Text("速度：%.1f km/h ｜ 遥测 revision %u", app.telemetry.speed * 3.6f, app.telemetry.pluginRevision);
            ::ImGui::Text("发动机 / 变速箱 / 车厢 / 底盘 / 车轮磨损：%.2f%% / %.2f%% / %.2f%% / %.2f%% / %.2f%%",
                app.telemetry.wear[0] * 100.0f, app.telemetry.wear[1] * 100.0f,
                app.telemetry.wear[2] * 100.0f, app.telemetry.wear[3] * 100.0f, app.telemetry.wear[4] * 100.0f);
        } else ::ImGui::TextWrapped("遥测：%s", app.telemetryError.c_str());
        if (::ImGui::CollapsingHeader("手动油量和损伤扫描")) {
            renderValuePanel(app, PanelKind::Fuel, "油量（手动扫描）", "扫描后根据游戏变化继续收窄。",
                &app.fuelType, app.fuelCurrent, app.fuelTarget, &app.fuelLock);
            renderValuePanel(app, PanelKind::Damage, "损伤（手动扫描）", "确认类型及地址后再写入。",
                &app.damageType, app.damageCurrent, app.damageTarget, &app.damageLock);
        }
        return;
    }
    if (app.vehicleLocker && (app.autoFuelLock || app.autoDamageLock)) {
        const std::string liveStatus = app.vehicleLocker->status();
        if (app.autoFuelLock) app.fuelStatus = liveStatus;
        if (app.autoDamageLock) app.damageStatus = liveStatus;
    }

    // --- 一键车辆状态锁定 Card ---
    ::ImGui::BeginChild("##auto_vehicle", ImVec2(0, 0),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ 车辆状态保持");
    ::ImGui::Separator();
    ::ImGui::Spacing();

    bool fuel = app.autoFuelLock;
    if (::ImGui::Checkbox("无限油量", &fuel)) {
        toggleAutoVehicleLock(app, true, fuel);
    }
    ::ImGui::SameLine(380.0f);
    if (app.autoFuelLock) {
        ::ImGui::TextColored(ImVec4(0.22f, 0.85f, 0.52f, 1.0f), "[ 已锁定 ]");
    } else {
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "[ 未开启 ]");
    }

    bool damage = app.autoDamageLock;
    if (::ImGui::Checkbox("车辆无损", &damage)) {
        toggleAutoVehicleLock(app, false, damage);
    }
    ::ImGui::SameLine(380.0f);
    if (app.autoDamageLock) {
        ::ImGui::TextColored(ImVec4(0.22f, 0.85f, 0.52f, 1.0f), "[ 已锁定 ]");
    } else {
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "[ 未开启 ]");
    }

    ::ImGui::Spacing();
    if (::ImGui::Button(" 全部开启 ")) {
        if (!app.autoFuelLock) toggleAutoVehicleLock(app, true, true);
        if (!app.autoDamageLock) toggleAutoVehicleLock(app, false, true);
    }
    ::ImGui::SameLine();
    if (::ImGui::Button(" 全部关闭 ")) {
        if (app.autoFuelLock) toggleAutoVehicleLock(app, true, false);
        if (app.autoDamageLock) toggleAutoVehicleLock(app, false, false);
    }
    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.75f, 1.0f), "油量：%s", app.fuelStatus.c_str());
    ::ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.75f, 1.0f), "损伤：%s", app.damageStatus.c_str());
    ::ImGui::EndChild();

    ::ImGui::Spacing();

    // --- 发动机动力调校 Card ---
    ::ImGui::BeginChild("##engine_power", ImVec2(0, 0),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ 发动机动力");
    ::ImGui::Separator();
    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "%s", multiplayerSummary().c_str());
    if (!app.mpDetail.empty()) {
        ::ImGui::TextColored(app.mpBlocks ? ImVec4(0.95f, 0.40f, 0.40f, 1.0f)
                                          : ImVec4(0.40f, 0.80f, 0.60f, 1.0f),
                             "联机保护状态：%s", app.mpDetail.c_str());
    }
    ::ImGui::Spacing();

    ::ImGui::TextUnformatted("倍率");
    ::ImGui::SameLine();
    static const char* kPowerPills[] = {"原厂", "1.25×", "1.50×", "2.00×"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ::ImGui::SameLine();
        const bool active = (app.enginePowerOption == i);
        if (active) {
            ::ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.95f, 1.0f));
        }
        if (::ImGui::Button(kPowerPills[i])) {
            applyEnginePower(app, i, app.engineRaiseLimit);
        }
        if (active) {
            ::ImGui::PopStyleColor();
        }
    }

    ::ImGui::Spacing();
    bool raise = app.engineRaiseLimit;
    if (::ImGui::Checkbox("转速上限 +10%", &raise)) {
        applyEnginePower(app, app.enginePowerOption, raise);
    }
    ::ImGui::SameLine(380.0f);
    if (::ImGui::Button(" ↩ 恢复原厂 ")) {
        applyEnginePower(app, 0, false);
    }

    if (app.engineTuner) {
        const std::string live = app.engineTuner->status();
        if (app.engineTuner->active()) {
            app.engineStatus = live;
        } else if (app.enginePowerOption != 0) {
            app.enginePowerOption = 0;
            app.engineRaiseLimit = false;
            app.engineStateText = app.engineTuner->exportState();
            app.engineStatus = live;
        }
        // 如实区分恢复记录状态：修改仍存在 / 尚未核对 / 对象已明确失效 / 已确认恢复
        const char* recordState = "已确认恢复（无待恢复记录）";
        ImVec4 recordColor(0.45f, 0.85f, 0.60f, 1.0f);
        if (app.engineTuner->hasUnverifiedImportedRecord()) {
            recordState = "上次运行的恢复记录尚未核对（已保留；不会把放大值当原厂值）";
            recordColor = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
        } else if (app.engineTuner->active()) {
            recordState = "修改仍存在（后台保持中）";
            recordColor = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
        } else if (app.engineTuner->hasInvalidatedRecord()) {
            recordState = "对象已明确失效（数值与本程序记录不符，记录已丢弃且未写入）";
            recordColor = ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
        }
        ::ImGui::TextColored(recordColor, "恢复记录：%s", recordState);
    }
    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.40f, 0.78f, 0.95f, 1.0f), "当前状态：%s", app.engineStatus.c_str());
    ::ImGui::EndChild();

    ::ImGui::Spacing();
    if (::ImGui::CollapsingHeader("高级：手动扫描（兼容性排查用）")) {
        ::ImGui::TextWrapped("全内存扫描会明显增加游戏内存压力，建议先存档。");
        ::ImGui::Spacing();
        renderValuePanel(app, PanelKind::Fuel, "手动油量扫描", "仅供兼容性排查。",
                         &app.fuelType, app.fuelCurrent, app.fuelTarget, &app.fuelLock);
        ::ImGui::Spacing();
        renderValuePanel(app, PanelKind::Damage, "手动损坏扫描", "仅供兼容性排查。",
                         &app.damageType, app.damageCurrent, app.damageTarget, &app.damageLock);
    }
}

void renderScannerTabImpl(AppState& app) {
    ::ImGui::TextUnformatted("类型");
    ::ImGui::SameLine();
    typeCombo("##stype", &app.scanType);
    if (vtypeInfo(app.scanType).isFloat) {
        ::ImGui::SameLine();
        ::ImGui::TextUnformatted("  容差");
        ::ImGui::SameLine();
        ::ImGui::SetNextItemWidth(80);
        ::ImGui::InputText("##stolerance", app.scanTolerance, sizeof(app.scanTolerance),
                           ImGuiInputTextFlags_CharsDecimal);
    }
    ::ImGui::SameLine();
    ::ImGui::TextUnformatted("  数值");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(160);
    ::ImGui::InputText("##svalue", app.scanValue, 32, ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button("首次扫描", ImVec2(110, 0))) startFirstScan(app, PanelKind::Scanner);
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    ::ImGui::TextUnformatted("对齐");
    ::ImGui::SameLine();
    {
        int alignIndex = 2;
        for (int i = 0; i < 4; ++i) {
            if ((1 << i) == app.scanAlign) alignIndex = i;
        }
        ::ImGui::SetNextItemWidth(60);
        if (::ImGui::Combo("##align", &alignIndex, "1\0" "2\0" "4\0" "8\0")) {
            app.scanAlign = 1 << alignIndex;
        }
    }
    ::ImGui::SameLine();
    ::ImGui::Checkbox("含超大内存区(慢很多)", &app.scanIncludeBig);

    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("再次扫描：");
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button("等于新数值", ImVec2(100, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Exact);
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("数值变了", ImVec2(90, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Changed);
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("数值没变", ImVec2(90, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Unchanged);
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("变大了", ImVec2(80, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Increased);
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("变小了", ImVec2(80, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Decreased);
    }
    ::ImGui::EndDisabled();

    // --- 扫描器：写入行与结果表格 ---
    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("写入值");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(160);
    ::ImGui::InputText("##snvalue", app.scanNewValue, 32, ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button("写入选中行", ImVec2(120, 0))) {
        ScanSession* session = app.scanSession.get();
        double value = 0;
        if (session && !session->empty() && !app.scanSelection.empty() &&
            parseNumber(app.scanNewValue, vtypeInfo(session->type()).isFloat, &value)) {
            const bool isFloat = vtypeInfo(session->type()).isFloat;
            int written = 0;
            for (int idx : app.scanSelection) {
                if (idx < 0 || idx >= (int)session->addresses().size()) continue;
                uint64_t addr = session->addresses()[(size_t)idx];
                bool ok = isFloat ? app.mem.writeDouble(addr, session->type(), value)
                                  : app.mem.writeInt(addr, session->type(), (int64_t)value);
                if (ok) ++written;
            }
            app.scanStatus = fmt("已写入选中 %d 行中的 %d 行 = %s",
                                 (int)app.scanSelection.size(), written, app.scanNewValue);
            logLine(app.scanStatus);
        } else {
            app.scanStatus = "请先扫描、选中行，并填写写入值";
        }
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("锁定选中行", ImVec2(120, 0))) {
        ScanSession* session = app.scanSession.get();
        double value = 0;
        if (session && !session->empty() && !app.scanSelection.empty() && app.freezer &&
            parseNumber(app.scanNewValue, vtypeInfo(session->type()).isFloat, &value)) {
            int n = 0;
            for (int idx : app.scanSelection) {
                if (idx < 0 || idx >= (int)session->addresses().size()) continue;
                app.freezer->add(session->addresses()[(size_t)idx], session->type(), value);
                ++n;
            }
            app.scanStatus = fmt("已锁定 %d 行 = %s", n, app.scanNewValue);
            logLine(app.scanStatus);
        } else {
            app.scanStatus = "请先扫描、选中行，并填写要锁定的数值";
        }
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("选中行全选/清空", ImVec2(150, 0))) {
        ScanSession* session = app.scanSession.get();
        if (session) {
            if (app.scanSelection.empty()) {
                int n = (int)(session->count() < 500 ? session->count() : 500);
                for (int i = 0; i < n; ++i) app.scanSelection.insert(i);
            } else {
                app.scanSelection.clear();
            }
        }
    }
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    if (::ImGui::Button("全部解锁", ImVec2(100, 0))) {
        if (app.freezer) app.freezer->clear();
        app.moneyLock = false;
        app.xpLock = false;
        app.fuelLock = false;
        app.damageLock = false;
        app.scanStatus = "已解除全部锁定";
        logLine(app.scanStatus);
    }
    ::ImGui::SameLine();
    ::ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.scanStatus.c_str());

    ::ImGui::Spacing();
    ScanSession* session = app.scanSession.get();
    if (!app.busy.load() && session && !session->empty()) {
        auto rows = session->snapshot(500);
        int rowIndex = 0;
        if (::ImGui::BeginTable("##results", 3,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                                ImVec2(0, 260))) {
            ::ImGui::TableSetupColumn("内存地址", ImGuiTableColumnFlags_WidthFixed, 190);
            ::ImGui::TableSetupColumn("当前值", ImGuiTableColumnFlags_WidthFixed, 170);
            ::ImGui::TableSetupColumn("上一次的值", ImGuiTableColumnFlags_WidthFixed, 170);
            ::ImGui::TableHeadersRow();
            for (const auto& row : rows) {
                const uint64_t addr = std::get<0>(row);
                const Value& cur = std::get<1>(row);
                const Value& prev = std::get<2>(row);
                ::ImGui::TableNextRow();
                ::ImGui::TableSetColumnIndex(0);
                bool selected = app.scanSelection.count(rowIndex) > 0;
                ::ImGui::PushID(rowIndex);
                if (::ImGui::Selectable(hexAddr(addr).c_str(), selected,
                                        ImGuiSelectableFlags_SpanAllColumns)) {
                    if (selected) app.scanSelection.erase(rowIndex);
                    else app.scanSelection.insert(rowIndex);
                }
                ::ImGui::PopID();
                ::ImGui::TableSetColumnIndex(1);
                ::ImGui::TextUnformatted(valueToString(cur).c_str());
                ::ImGui::TableSetColumnIndex(2);
                ::ImGui::TextUnformatted(valueToString(prev).c_str());
                ++rowIndex;
            }
            ::ImGui::EndTable();
        }
        ::ImGui::Text("共 %llu 处候选地址（表格显示前 500 处）",
                      (unsigned long long)session->count());
    }
}

// ==========================================================================
// 地图传送（控制台 goto）
// ==========================================================================
namespace {

// 只保留控制台能安全输入、goto 能接受的字符（城市内部名与坐标）。
std::string sanitizeTeleportTarget(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
            c == ';' || c == '-' || c == ' ') {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

void loadMapCities(AppState& app) {
    if (app.slots.empty()) app.slots = listSlots();
    if (app.slots.empty()) {
        app.teleportNote = "没有找到存档，无法读取城市列表。";
        return;
    }
    const int index = (app.selectedSlot >= 0 && app.selectedSlot < (int)app.slots.size())
                          ? app.selectedSlot
                          : 0;
    const SaveSlot& slot = app.slots[(size_t)index];
    std::vector<std::string> cities;
    std::string error;
    if (!listMapCities(slot.gameSii, &cities, &error)) {
        app.teleportNote = "读取城市列表失败：" + error;
        logLine(app.teleportNote);
        return;
    }
    app.mapCities = std::move(cities);
    app.mapCitySelected = app.mapCities.empty() ? -1 : 0;
    app.teleportNote = fmt("已从「%s」读到 %d 个城市内部名（来自存档里的仓库单位名）。",
                           slot.label.c_str(), (int)app.mapCities.size());
    logLine(app.teleportNote);
}

void teleportTo(AppState& app, const std::string& target) {
    const std::string clean = sanitizeTeleportTarget(target);
    if (clean.empty()) {
        app.teleportNote = "请先填写城市名或坐标（坐标格式 X;Y;Z，可带 ;A;E）。";
        return;
    }
    app.teleportNote = "已发送传送命令：goto " + clean;
    logLine(app.teleportNote);
    sendConsole(app, "goto " + clean);
}

void saveTeleportSpot(AppState& app) {
    const std::string name = sanitizeTeleportTarget(app.spotName);
    std::string target = sanitizeTeleportTarget(app.coordInput);
    if (target.empty()) target = sanitizeTeleportTarget(app.cityInput);
    if (target.empty() && app.mapCitySelected >= 0 &&
        app.mapCitySelected < (int)app.mapCities.size()) {
        target = app.mapCities[(size_t)app.mapCitySelected];
    }
    if (name.empty() || target.empty()) {
        app.teleportNote = "收藏需要一个名字和一个目标（城市名或坐标）。";
        return;
    }
    app.teleportSpots.emplace_back(name, target);
    app.spotName[0] = 0;
    saveSettings(app);
    app.teleportNote = fmt("已收藏「%s」→ %s", name.c_str(), target.c_str());
    logLine(app.teleportNote);
}

void removeTeleportSpot(AppState& app, int index) {
    if (index < 0 || index >= (int)app.teleportSpots.size()) return;
    app.teleportSpots.erase(app.teleportSpots.begin() + index);
    saveSettings(app);
}

namespace {

void renderTeleportSection(AppState& app) {
    if (!::ImGui::CollapsingHeader("地图传送（控制台 goto）", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ::ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f),
                         "传送只移动相机；调好高度后按 Ctrl+F9 把卡车落到该处。");
    ::ImGui::Spacing();

    if (::ImGui::Button("读取城市列表（解密存档）", ImVec2(200, 0))) loadMapCities(app);
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(140);
    ::ImGui::InputTextWithHint("##cityfilter", "筛选城市名", app.cityFilter, sizeof(app.cityFilter));
    ::ImGui::SameLine();
    ::ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "共 %d 个城市",
                         (int)app.mapCities.size());

    if (!app.mapCities.empty()) {
        std::vector<int> filtered;
        const std::string filter = sanitizeTeleportTarget(app.cityFilter);
        for (size_t i = 0; i < app.mapCities.size(); ++i) {
            if (filter.empty() || app.mapCities[i].find(filter) != std::string::npos) {
                filtered.push_back((int)i);
            }
        }
        const std::string preview = (app.mapCitySelected >= 0 &&
                                     app.mapCitySelected < (int)app.mapCities.size())
                                        ? app.mapCities[(size_t)app.mapCitySelected]
                                        : std::string("(未选择)");
        ::ImGui::SetNextItemWidth(220);
        if (::ImGui::BeginCombo("##city", preview.c_str())) {
            for (int index : filtered) {
                const bool selected = (index == app.mapCitySelected);
                if (::ImGui::Selectable(app.mapCities[(size_t)index].c_str(), selected)) {
                    app.mapCitySelected = index;
                }
                if (selected) ::ImGui::SetItemDefaultFocus();
            }
            ::ImGui::EndCombo();
        }
        ::ImGui::SameLine();
        if (::ImGui::Button("传送到该城市", ImVec2(130, 0))) {
            if (app.mapCitySelected >= 0 && app.mapCitySelected < (int)app.mapCities.size()) {
                teleportTo(app, app.mapCities[(size_t)app.mapCitySelected]);
            }
        }
    }

    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("城市名");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(160);
    ::ImGui::InputTextWithHint("##cityinput", "例：berlin / calais", app.cityInput,
                               sizeof(app.cityInput));
    ::ImGui::SameLine();
    if (::ImGui::Button("传送（城市）", ImVec2(110, 0))) teleportTo(app, app.cityInput);

    ::ImGui::SameLine(430.0f);
    ::ImGui::TextUnformatted("坐标");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(180);
    ::ImGui::InputTextWithHint("##coords", "例：366.0;46.7;617.2", app.coordInput,
                               sizeof(app.coordInput));
    ::ImGui::SameLine();
    if (::ImGui::Button("传送（坐标）", ImVec2(110, 0))) teleportTo(app, app.coordInput);

    ::ImGui::Spacing();
    if (::ImGui::Button("导航起点", ImVec2(90, 0))) teleportTo(app, "nav_start");
    ::ImGui::SameLine();
    if (::ImGui::Button("导航终点", ImVec2(90, 0))) teleportTo(app, "nav_end");
    ::ImGui::SameLine();
    if (::ImGui::Button("下一个导航点", ImVec2(110, 0))) teleportTo(app, "nav_next");
    ::ImGui::SameLine();
    if (::ImGui::Button("上一个导航点", ImVec2(110, 0))) teleportTo(app, "nav_prev");
    ::ImGui::SameLine();
    if (::ImGui::Button("返回上一位置", ImVec2(110, 0))) teleportTo(app, "back");
    ::ImGui::SameLine();
    if (::ImGui::Button("瞬移车辆到相机处 (Ctrl+F9)", ImVec2(220, 0))) {
        DWORD pid = app.pid;
        if (!pid) {
            std::wstring exe;
            findProcess(selectedProcessName(), &pid, &exe);
        }
        if (pid) {
            uintptr_t hwnd = findGameWindow(pid);
            if (hwnd && activateWindow(hwnd)) {
                BYTE ctrlScan = (BYTE)::MapVirtualKeyW(VK_CONTROL, 0);
                BYTE f9Scan = (BYTE)::MapVirtualKeyW(VK_F9, 0);
                ::keybd_event(VK_CONTROL, ctrlScan, 0, 0);
                ::Sleep(80);
                ::keybd_event(VK_F9, f9Scan, 0, 0);
                ::Sleep(120);
                ::keybd_event(VK_F9, f9Scan, KEYEVENTF_KEYUP, 0);
                ::Sleep(50);
                ::keybd_event(VK_CONTROL, ctrlScan, KEYEVENTF_KEYUP, 0);
                ::Sleep(100);
                logLine("已发送 [Ctrl+F9] 瞬移卡车到相机位置");
            }
        }
    }

    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("收藏点");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(120);
    ::ImGui::InputTextWithHint("##spotname", "名字", app.spotName, sizeof(app.spotName));
    ::ImGui::SameLine();
    if (::ImGui::Button("收藏当前目标", ImVec2(120, 0))) saveTeleportSpot(app);
    for (int i = 0; i < (int)app.teleportSpots.size(); ++i) {
        ::ImGui::PushID(i);
        if (::ImGui::Button("传送", ImVec2(60, 0))) teleportTo(app, app.teleportSpots[(size_t)i].second);
        ::ImGui::SameLine();
        if (::ImGui::Button("删除", ImVec2(60, 0))) {
            removeTeleportSpot(app, i);
            ::ImGui::PopID();
            break;
        }
        ::ImGui::SameLine();
        ::ImGui::TextUnformatted(fmt("%s → %s", app.teleportSpots[(size_t)i].first.c_str(),
                                     app.teleportSpots[(size_t)i].second.c_str()).c_str());
        ::ImGui::PopID();
    }
    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", app.teleportNote.c_str());
}

}  // namespace

// ==========================================================================
// 联网 / 联运模式
// ==========================================================================
void refreshConvoy(AppState& app) {
    MultiplayerStatus status = checkMultiplayer(app.pid, true);
    app.mpBlocks = status.blocksWrite();
    app.mpDetail = status.detail;
    if (isConvoyCheckEnabled()) {
        app.convoy = readConvoySession();
        app.convoyNote = app.convoy.note;
        if (app.convoy.active && !app.convoy.players.empty()) {
            std::string names;
            for (const auto& player : app.convoy.players) {
                if (!names.empty()) names += "、";
                names += player.name;
                if (player.you) names += "(你)";
            }
            app.convoyNote += "：" + names;
        }
    } else {
        app.convoy = ConvoySession{};
        app.convoyNote = "联运检测已关闭。点「读取玩家列表」可手动读取日志中的会话。";
    }
    logLine("联机状态：" + app.mpDetail + (isConvoyCheckEnabled() ? ("；" + app.convoy.note) : ""));
}

void setMultiplayerPolicy(AppState& app, int policy) {
    app.mpPolicy = (policy == 1) ? 1 : 0;
    saveSettings(app);
    logLine(app.mpPolicy == 1
                ? "已切换为「联运模式」：联机时放开控制台/传送/自由相机，写入类功能仍然禁用"
                : "已切换为「严格模式」：联机时禁用所有功能");
}

std::string multiplayerGateForSafeFeature(const AppState& app) {
    DWORD pid = app.pid;
    if (!pid) {
        std::wstring exe;
        findProcess(selectedProcessName(), &pid, &exe);
    }
    MultiplayerStatus status = checkMultiplayer(pid);
    if (status.kind == MultiplayerKind::TruckersMp) {
        return "TruckersMP 环境下已禁用该功能（对方有反作弊，容易被封号）";
    }
    if (isConvoyCheckEnabled() && status.kind == MultiplayerKind::Convoy && app.mpPolicy != 1) {
        return "检测到官方 Convoy 会话：默认禁用。需要的话请在「联运模式」面板里显式切换为联运模式。";
    }
    return std::string();
}

namespace {

void renderConvoySection(AppState& app) {
    if (!::ImGui::CollapsingHeader("联运模式（Convoy / 联机）", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    bool checkConvoy = app.checkConvoy;
    if (::ImGui::Checkbox("启用官方联运 (Convoy) 会话检测", &checkConvoy)) {
        app.checkConvoy = checkConvoy;
        setConvoyCheckEnabled(checkConvoy);
        refreshConvoy(app);
        saveSettings(app);
    }
    if (!app.checkConvoy) {
        ::ImGui::SameLine();
        ::ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "（检测已关闭）");
    }
    ::ImGui::Spacing();

    if (::ImGui::Button("刷新联机状态", ImVec2(130, 0))) refreshConvoy(app);
    ::ImGui::SameLine();
    if (::ImGui::Button("读取玩家列表", ImVec2(130, 0))) {
        app.convoy = readConvoySession();
        app.convoyNote = app.convoy.note;
    }
    ::ImGui::SameLine();
    ::ImGui::TextColored(app.mpBlocks ? ImVec4(1.0f, 0.6f, 0.6f, 1.0f)
                                      : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                         "%s", app.mpDetail.empty() ? "还没检测联机状态" : app.mpDetail.c_str());

    if (app.convoy.active) {
        ::ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s（开始于 %s）",
                             app.convoy.note.c_str(),
                             app.convoy.startedAt.empty() ? "未知" : app.convoy.startedAt.c_str());
        if (app.convoy.players.empty()) {
            ::ImGui::TextUnformatted("（还没有解析到玩家，可能刚进会话）");
        } else {
            for (const auto& player : app.convoy.players) {
                ::ImGui::BulletText("%s  (client %d)%s", player.name.c_str(), player.clientId,
                                    player.you ? "  ← 你" : "");
            }
        }
    } else {
        ::ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.convoyNote.c_str());
    }

    if (app.checkConvoy) {
        ::ImGui::Spacing();
        int policy = app.mpPolicy;
        if (::ImGui::RadioButton("严格模式（推荐）：联机时全部禁用", &policy, 0)) {
            setMultiplayerPolicy(app, 0);
        }
        if (::ImGui::RadioButton("联运模式：联机时放开不写内存的功能", &policy, 1)) {
            setMultiplayerPolicy(app, 1);
        }
    }
    ::ImGui::Spacing();
    const std::string gate = multiplayerGateForSafeFeature(app);
    ::ImGui::TextColored(gate.empty() ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f)
                                      : ImVec4(1.0f, 0.75f, 0.5f, 1.0f),
                         "安全功能当前：%s", gate.empty() ? "可用" : gate.c_str());
    ::ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                         "写入功能：%s",
                         app.mpBlocks ? "联机中已禁用" : (app.checkConvoy ? "可用（单机）" : "可用（联运检测已关闭）"));
}

void renderFlyModeSection(AppState& app);

void renderConsoleTabImpl(AppState& app) {
    ::ImGui::TextUnformatted("控制台按键");
    ::ImGui::SameLine();
    const auto& options = consoleKeyOptions();
    int currentOption = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i].vk == app.consoleKey) currentOption = (int)i;
    }
    std::string preview = options[(size_t)currentOption].label;
    ::ImGui::SetNextItemWidth(170);
    if (::ImGui::BeginCombo("##ckey", preview.c_str())) {
        for (size_t i = 0; i < options.size(); ++i) {
            bool selected = ((int)i == currentOption);
            if (::ImGui::Selectable(options[i].label, selected)) {
                app.consoleKey = options[i].vk;
                saveSettings(app);
            }
            if (selected) ::ImGui::SetItemDefaultFocus();
        }
        ::ImGui::EndCombo();
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("从 controls.sii 自动识别")) detectConsoleKey(app);
    ::ImGui::SameLine();
    if (::ImGui::Button("检测/开启控制台(config.cfg)")) checkConsoleConfig(app);
    if (!app.consoleNote.empty()) {
        ::ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.55f, 1.0f), "%s", app.consoleNote.c_str());
    }
    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("命令");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(-140);
    ::ImGui::InputText("##cmd", app.consoleCmd, sizeof(app.consoleCmd));
    ::ImGui::SameLine();
    if (::ImGui::Button("发送到游戏", ImVec2(120, 0))) sendConsole(app, app.consoleCmd);

    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("常用命令");
    struct Preset {
        const char* label;
        const char* command;
    };
    static const Preset kPresets[] = {
        {"时间 08:00", "g_set_time 8 0"},   {"时间 20:00", "g_set_time 20 0"},
        {"关闭警察", "g_police 0"},         {"开启警察", "g_police 1"},
        {"关闭疲劳", "g_fatigue 0"},        {"开启疲劳", "g_fatigue 1"},
        {"收入 x5", "g_income_factor 5.0"}, {"收入 x1", "g_income_factor 1.0"},
        {"清空交通", "g_traffic 0"},        {"恢复交通", "g_traffic 1"},
        {"显示 FPS", "g_fps 1"},            {"关闭 FPS", "g_fps 0"},
    };
    const int perRow = 6;
    for (int i = 0; i < (int)(sizeof(kPresets) / sizeof(kPresets[0])); ++i) {
        if (i % perRow) ::ImGui::SameLine();
        if (::ImGui::Button(kPresets[i].label, ImVec2(110, 0))) {
            sendConsole(app, kPresets[i].command);
        }
    }
    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.55f, 1.0f),
                         "发送会切换游戏窗口到前台，驾驶中请先停车。");
    ::ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                         "需要 config.cfg：uset g_console \"1\" 与 uset g_developer \"1\"。");

    ::ImGui::Spacing();
    ::ImGui::Separator();
    ::ImGui::Spacing();
    renderConvoySection(app);
    renderFlyModeSection(app);
    renderTeleportSection(app);
}

void renderFlyModeSection(AppState& app) {
    if (::ImGui::CollapsingHeader("飞行模式（自由相机）", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (::ImGui::Button("检查 config.cfg", ImVec2(140, 0))) refreshFlyMode(app);
        ::ImGui::SameLine();
        if (::ImGui::Button("从 controls.sii 识别相机按键", ImVec2(220, 0))) detectCameraKeys(app);
        ::ImGui::SameLine();
        if (::ImGui::Button("写入飞行模式设置", ImVec2(160, 0))) enableFlyMode(app);
        ::ImGui::Spacing();
        ::ImGui::TextWrapped("%s", app.flyNote.c_str());
        if (app.cameraLoaded) {
            ::ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                                 "相机按键：开关 %s · 前进 %s · 后退 %s · 左 %s · 右 %s · 上升 %s · "
                                 "下降 %s",
                                 keyLabel(app.camera.toggle).c_str(),
                                 keyLabel(app.camera.forward).c_str(),
                                 keyLabel(app.camera.back).c_str(),
                                 keyLabel(app.camera.left).c_str(),
                                 keyLabel(app.camera.right).c_str(),
                                 keyLabel(app.camera.up).c_str(),
                                 keyLabel(app.camera.down).c_str());
        }
        ::ImGui::Spacing();
        ::ImGui::TextUnformatted("飞行速度 g_flyspeed");
        ::ImGui::SameLine();
        ::ImGui::SetNextItemWidth(90);
        ::ImGui::InputText("##flyspeed", app.flySpeed, sizeof(app.flySpeed),
                           ImGuiInputTextFlags_CharsDecimal);
        ::ImGui::SameLine();
        if (::ImGui::Button("立即应用", ImVec2(90, 0))) setFlySpeed(app, app.flySpeed);
        static const char* kSpeeds[] = {"50", "100", "300", "1000", "3000"};
        for (const char* speed : kSpeeds) {
            ::ImGui::SameLine();
            if (::ImGui::Button(speed, ImVec2(60, 0))) setFlySpeed(app, speed);
        }
        ::ImGui::Spacing();
        if (::ImGui::Button("切换自由相机（进入/退出视角）", ImVec2(220, 0))) toggleFreeCamera(app);
        ::ImGui::SameLine();
        if (::ImGui::Button("瞬移车辆到当前自由相机位置 (Ctrl+F9)", ImVec2(260, 0))) {
            DWORD pid = app.pid;
            if (!pid) {
                std::wstring exe;
                findProcess(selectedProcessName(), &pid, &exe);
            }
            if (pid) {
                uintptr_t hwnd = findGameWindow(pid);
                if (hwnd && activateWindow(hwnd)) {
                    BYTE ctrlScan = (BYTE)::MapVirtualKeyW(VK_CONTROL, 0);
                    BYTE f9Scan = (BYTE)::MapVirtualKeyW(VK_F9, 0);
                    ::keybd_event(VK_CONTROL, ctrlScan, 0, 0);
                    ::Sleep(80);
                    ::keybd_event(VK_F9, f9Scan, 0, 0);
                    ::Sleep(120);
                    ::keybd_event(VK_F9, f9Scan, KEYEVENTF_KEYUP, 0);
                    ::Sleep(50);
                    ::keybd_event(VK_CONTROL, ctrlScan, KEYEVENTF_KEYUP, 0);
                    ::Sleep(100);
                    logLine("已发送 [Ctrl+F9] 瞬移卡车到相机位置");
                }
            }
        }
        ::ImGui::SameLine();
        if (::ImGui::Button("切回第一人称驾驶室 (按1)", ImVec2(180, 0))) {
            DWORD pid = app.pid;
            if (!pid) {
                std::wstring exe;
                findProcess(selectedProcessName(), &pid, &exe);
            }
            if (pid) {
                sendKeyTap('1', pid);
            }
        }
    }
}

void renderSaveTabImpl(AppState& app) {
    ::ImGui::BeginDisabled(app.busy);
    ::ImGui::TextUnformatted("本地与 Steam 云存档；复制卡车支持自动解密和转换，明文存档可改现金 / 经验（需先退出游戏）。");
    ::ImGui::Spacing();
    if (::ImGui::Button("刷新存档列表", ImVec2(120, 0))) refreshSaves(app);
    ::ImGui::SameLine();
    if (::ImGui::Button("备份选中存档", ImVec2(120, 0))) backupSelectedSlot(app);
    ::ImGui::SameLine();
    if (::ImGui::Button("解密导出选中", ImVec2(130, 0))) exportSelectedSlot(app);
    if (::ImGui::IsItemHovered()) ::ImGui::SetTooltip("保留解密后的格式：BSII / 3nK 仍为二进制。");
    ::ImGui::SameLine();
    if (::ImGui::Button("导出可读 SII", ImVec2(150, 0))) exportReadableSelectedSlot(app);
    if (::ImGui::IsItemHovered()) ::ImGui::SetTooltip("自动解密并把 BSII v3 转成可读文本；已是 SII 文本时原样保留。");
    if (::ImGui::Button("打开备份文件夹", ImVec2(130, 0))) {
        std::wstring root = backupRoot();
        ::CreateDirectoryW(root.c_str(), nullptr);
        ::ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("打开导出文件夹", ImVec2(130, 0))) {
        const std::wstring exports = documentsDir() + L"\\exports";
        if (!isDirSegment(exports)) ::CreateDirectoryW(exports.c_str(), nullptr);
        if (!isDirSegment(exports)) {
            app.saveNote = "打开导出文件夹失败：目标不是可用目录";
            logLine(app.saveNote);
        } else if ((INT_PTR)::ShellExecuteW(nullptr, L"open", exports.c_str(), nullptr, nullptr,
                                            SW_SHOWNORMAL) <= 32) {
            app.saveNote = "打开导出文件夹失败：系统无法打开该目录";
            logLine(app.saveNote);
        }
    }

    ::ImGui::SameLine();
    if (::ImGui::Button("复制为新存档")) {
        if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) app.saveNote = "请先选择源存档";
        else {
            std::string error;
            if (copyBlockedByRunningGame(selectedGame(), &error)) app.saveNote = error;
            else {
                app.copyRequest = {selectedGame(), app.slots[(size_t)app.selectedSlot], {}};
                const auto& name = app.copyRequest.source.displayName;
                ::strncpy_s(app.copyName, (name.empty() ? std::string("存档副本") : name + " 副本").c_str(), _TRUNCATE);
                app.showCopyDialog = true;
            }
        }
    }
    if (app.showCopyDialog) { ::ImGui::OpenPopup("复制为新存档##copy"); app.showCopyDialog = false; }
    if (::ImGui::BeginPopupModal("复制为新存档##copy", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (app.copyRequest.source.slotDir.empty()) ::ImGui::CloseCurrentPopup();
        ::ImGui::Text("游戏：%s", gameDescriptor(app.copyRequest.game).shortName);
        ::ImGui::Text("档案：%s / 源槽位：%s", W2U(app.copyRequest.source.profileName).c_str(), W2U(app.copyRequest.source.slotName).c_str());
        ::ImGui::TextUnformatted("在同一档案创建空闲编号；保留全部存档文件和游戏进度。名称最多 64 个字符。");
        ::ImGui::SetNextItemWidth(420);
        ::ImGui::InputText("新存档名称", app.copyName, sizeof(app.copyName));
        std::string escaped, error;
        bool valid = siiEscapeName(app.copyName, &escaped, &error);
        if (!valid) ::ImGui::TextWrapped("%s", error.c_str());
        ::ImGui::BeginDisabled(!valid);
        if (::ImGui::Button("开始复制")) { startSaveCopy(app); ::ImGui::CloseCurrentPopup(); }
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        if (::ImGui::Button("取消")) ::ImGui::CloseCurrentPopup();
        ::ImGui::EndPopup();
    }

    ::ImGui::Spacing();
    if (::ImGui::Button("跨存档复制卡车")) {
        if(app.truckTransferUiProbe.capture) app.truckTransferUiProbe.clicked=true;
        openTruckTransfer(app);
    }
    if(app.truckTransferUiProbe.capture) {
        const auto lo=::ImGui::GetItemRectMin(),hi=::ImGui::GetItemRectMax();
        app.truckTransferUiProbe.buttonX=(lo.x+hi.x)/2;
        app.truckTransferUiProbe.buttonY=(lo.y+hi.y)/2;
    }
    if(app.busy && ::ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ::ImGui::SetTooltip("当前任务正在处理中，请完成或取消后再复制卡车。");
    ::ImGui::SameLine();
    ::ImGui::TextDisabled("直接选择现有存档；退出对应游戏后使用");
    ::ImGui::EndDisabled();
    renderTruckTransferDialog(app);
    ::ImGui::BeginDisabled(app.busy);
    ::ImGui::Spacing();
    if (::ImGui::Button("拆分为独立档案")) openProfileSplit(app);
    ::ImGui::SameLine();
    ::ImGui::TextDisabled("同一档案的多个存档 → 各自独立的本地档案");
    renderProfileSplitDialog(app);
    ::ImGui::TextWrapped("%s",app.saveNote.c_str());
    if (::ImGui::BeginTable("##slots", 7,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                            ImVec2(0, 230))) {
        ::ImGui::TableSetupColumn("修改时间", ImGuiTableColumnFlags_WidthFixed, 150);
        ::ImGui::TableSetupColumn("档案", ImGuiTableColumnFlags_WidthFixed, 120);
        ::ImGui::TableSetupColumn("存档位", ImGuiTableColumnFlags_WidthFixed, 130);
        ::ImGui::TableSetupColumn("位置", ImGuiTableColumnFlags_WidthFixed, 70);
        ::ImGui::TableSetupColumn("格式", ImGuiTableColumnFlags_WidthFixed, 110);
        ::ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 90);
        ::ImGui::TableSetupColumn("存档名称", ImGuiTableColumnFlags_WidthStretch);
        ::ImGui::TableHeadersRow();
        for (int i = 0; i < (int)app.slots.size(); ++i) {
            const SaveSlot& slot = app.slots[(size_t)i];
            ::ImGui::TableNextRow();
            ::ImGui::TableSetColumnIndex(0);
            ::ImGui::PushID(i);
            bool selected = (i == app.selectedSlot);
            if (::ImGui::Selectable(localTimeString(slot.mtime).c_str(), selected,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                selectSlot(app, i);
            }
            ::ImGui::PopID();
            ::ImGui::TableSetColumnIndex(1);
            ::ImGui::TextUnformatted(W2U(slot.profileName).c_str());
            ::ImGui::TableSetColumnIndex(2);
            ::ImGui::TextUnformatted(W2U(slot.slotName).c_str());
            ::ImGui::TableSetColumnIndex(3);
            ::ImGui::TextUnformatted(slot.cloud ? "Steam云" : "本地");
            ::ImGui::TableSetColumnIndex(4);
            ::ImGui::TextUnformatted(slot.formatText.c_str());
            ::ImGui::TableSetColumnIndex(5);
            ::ImGui::TextUnformatted(formatSize(slot.size).c_str());
            ::ImGui::TableSetColumnIndex(6);
            ::ImGui::TextUnformatted(slot.displayName.empty() ? "—" : slot.displayName.c_str());
        }
        ::ImGui::EndTable();
    }

    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("明文存档数值修改");
    ::ImGui::SameLine();
    ::ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "（写入前自动备份）");
    ::ImGui::TextUnformatted("现金");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(150);
    ::ImGui::InputText("##smoney", app.saveMoney, sizeof(app.saveMoney),
                       ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    ::ImGui::TextUnformatted("经验");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(150);
    ::ImGui::InputText("##sxp", app.saveXp, sizeof(app.saveXp), ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    if (::ImGui::Button("写入明文存档", ImVec2(130, 0))) applyTextPatch(app);

    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("备份列表");
    ::ImGui::SameLine();
    if (::ImGui::Button("还原选中备份", ImVec2(120, 0))) restoreSelectedBackup(app);
    if (::ImGui::BeginTable("##backups", 4,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                            ImVec2(0, 150))) {
        ::ImGui::TableSetupColumn("备份时间", ImGuiTableColumnFlags_WidthFixed, 160);
        ::ImGui::TableSetupColumn("档案", ImGuiTableColumnFlags_WidthFixed, 120);
        ::ImGui::TableSetupColumn("存档位", ImGuiTableColumnFlags_WidthFixed, 130);
        ::ImGui::TableSetupColumn("备注", ImGuiTableColumnFlags_WidthFixed, 110);
        ::ImGui::TableHeadersRow();
        for (int i = 0; i < (int)app.backups.size(); ++i) {
            const BackupInfo& info = app.backups[(size_t)i];
            ::ImGui::TableNextRow();
            ::ImGui::TableSetColumnIndex(0);
            ::ImGui::PushID(10000 + i);
            bool selected = (i == app.selectedBackup);
            if (::ImGui::Selectable(W2U(info.time).c_str(), selected,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                app.selectedBackup = i;
            }
            ::ImGui::PopID();
            ::ImGui::TableSetColumnIndex(1);
            ::ImGui::TextUnformatted(W2U(info.profile).c_str());
            ::ImGui::TableSetColumnIndex(2);
            ::ImGui::TextUnformatted(W2U(info.slot).c_str());
            ::ImGui::TableSetColumnIndex(3);
            ::ImGui::TextUnformatted(W2U(info.label).c_str());
        }
        ::ImGui::EndTable();
    }
    ::ImGui::EndDisabled();
    if (app.busy && ::ImGui::Button("取消当前任务")) app.cancel = true;
}

void renderHelpTabImpl(AppState& app) {
    static const char* kHelp =
        "【前提】先启动游戏（单机），再点顶部「附加游戏」；改内存需要管理员权限。\n"
        "\n"
        "【改钱 / 经验】美卡：填写目标金额后直接改钱；欧卡首次需填写当前金额作为定位种子。\n"
        "　　　　　　　之后：直接填目标值再点一次即可。写的是游戏权威存储，无需锁定。\n"
        "【车辆】附加后直接勾选，无需填数值；换车、读档自动跟随当前车辆。\n"
        "【扫描器】任意数值都可扫描；向多个候选地址写入前会要求确认。\n"
        "【存档】备份 / 还原 / 解密导出（导出到 文档\\exports，不覆盖旧文件）/ 复制为新存档；"
        "写入与复制需先退出对应游戏。\n"
        "\n"
        "【加密存档为什么不能改】ScsC 头部含游戏自己的校验值，强写会损坏存档，\n"
        "因此只做只读解密。要持久化金额请改内存，再让游戏自动存档。\n"
        "\n"
        "【安全】只在单机使用；TruckersMP 与官方联运中会拒绝写入。\n"
        "　　　　本工具不修改游戏 exe，也不注入代码，只做内存与存档文件读写。";
    ::ImGui::TextWrapped("%s", kHelp);
    ::ImGui::Spacing();
    ::ImGui::Separator();
    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "全局游戏热键（无需切屏）：");
    bool hk = app.hotkeysEnabled;
    if (::ImGui::Checkbox("启用游戏内全局热键", &hk)) {
        app.hotkeysEnabled = hk;
        saveSettings(app);
    }
    ::ImGui::BulletText("Ctrl+F1 传送导航终点 · Ctrl+F2 无限油量 · Ctrl+F3 车辆无损");
    ::ImGui::BulletText("Ctrl+F4 发动机动力 · Ctrl+F5 时间 09:00 · Ctrl+Backspace 撤销传送");
    ::ImGui::Spacing();
    ::ImGui::Separator();
    ::ImGui::Spacing();
    ::ImGui::TextUnformatted("扫描时跳过超过");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(90);
    ::ImGui::DragFloat("##maxgb", &app.maxRegionGb, 0.1f, 0.0f, 64.0f, "%.1f GB");
    ::ImGui::SameLine();
    ::ImGui::TextUnformatted("的内存区（0 = 不跳过）");
    ::ImGui::SameLine();
    ::ImGui::TextUnformatted("   线程数");
    ::ImGui::SameLine();
    {
        int idx = 0;
        static const int kChoices[] = {1, 2, 4, 6, 8};
        for (int i = 0; i < 5; ++i) {
            if (kChoices[i] == app.workers) idx = i;
        }
        ::ImGui::SetNextItemWidth(70);
        if (::ImGui::Combo("##workers", &idx, "1\0" "2\0" "4\0" "6\0" "8\0")) {
            app.workers = kChoices[idx];
        }
    }
    ::ImGui::SameLine();
    if (::ImGui::Button("保存设置")) {
        saveSettings(app);
        logLine("设置已保存到 settings.ini");
    }
    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.procStatus.c_str());
    if (!app.memInfo.empty()) {
        ::ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.memInfo.c_str());
    }
}

void renderConfirmDialogs(AppState& app) {
    if (app.requestMultiConfirm) {
        ::ImGui::OpenPopup("确认批量修改");
        app.requestMultiConfirm = false;
    }
    if (::ImGui::BeginPopupModal("确认批量修改", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        PanelKind kind = (app.pendingMultiKind >= (int)PanelKind::Money &&
                          app.pendingMultiKind <= (int)PanelKind::Damage)
                             ? (PanelKind)app.pendingMultiKind
                             : PanelKind::Money;
        ScanSession* session = sessionFor(app, kind);
        const size_t count = session ? session->count() : 0;
        ::ImGui::TextColored(ImVec4(0.95f, 0.50f, 0.35f, 1.0f),
                             "当前仍有 %llu 个候选地址。",
                             (unsigned long long)count);
        ::ImGui::TextWrapped("批量修改可能把无关的游戏数据一起改掉。建议取消并继续收窄，直到只剩 1 个地址。");
        ::ImGui::Spacing();
        const char* confirmLabel = app.pendingMultiAction == 2 ? "仍然锁定全部候选"
                                                                : "仍然写入全部候选";
        ::ImGui::BeginDisabled(!session || count < 2);
        if (::ImGui::Button(confirmLabel, ImVec2(190, 0))) {
            if (app.pendingMultiAction == 2) {
                if (bool* state = lockFor(app, kind)) *state = true;
                togglePanelLock(app, kind, true, true);
            } else {
                writePanelValues(app, kind, true);
            }
            app.pendingMultiAction = 0;
            app.pendingMultiKind = -1;
            ::ImGui::CloseCurrentPopup();
        }
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        if (::ImGui::Button("取消，继续收窄", ImVec2(170, 0))) {
            if (app.pendingMultiAction == 2) {
                if (bool* state = lockFor(app, kind)) *state = false;
            }
            app.pendingMultiAction = 0;
            app.pendingMultiKind = -1;
            ::ImGui::CloseCurrentPopup();
        }
        ::ImGui::EndPopup();
    }

    if (app.requestRestoreConfirm) {
        ::ImGui::OpenPopup("确认还原存档");
        app.requestRestoreConfirm = false;
    }
    if (::ImGui::BeginPopupModal("确认还原存档", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        const BackupInfo* info =
            (app.selectedBackup >= 0 && app.selectedBackup < (int)app.backups.size())
                ? &app.backups[(size_t)app.selectedBackup]
                : nullptr;
        if (info) {
            ::ImGui::Text("档案：%s", W2U(info->profile).c_str());
            ::ImGui::Text("存档位：%s", W2U(info->slot).c_str());
            ::ImGui::Text("备份时间：%s", W2U(info->time).c_str());
        }
        DWORD gamePid = 0;
        const bool gameRunning = findProcess(selectedProcessName(), &gamePid, nullptr);
        if (gameRunning) {
            ::ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.35f, 1.0f),
                                 "%s仍在运行（PID %u），请先退出游戏。", selectedGameDescriptor().shortName, gamePid);
        } else {
            ::ImGui::TextWrapped("程序会先备份当前存档，再覆盖还原。请确认已暂停 Steam 云同步或了解冲突处理方式。");
        }
        ::ImGui::Spacing();
        ::ImGui::BeginDisabled(!info || gameRunning);
        if (::ImGui::Button("备份当前状态并还原", ImVec2(210, 0))) {
            performRestoreSelectedBackup(app);
            ::ImGui::CloseCurrentPopup();
        }
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        if (::ImGui::Button("取消", ImVec2(100, 0))) ::ImGui::CloseCurrentPopup();
        ::ImGui::EndPopup();
    }
}

}  // namespace

void renderApp(AppState& app) {
    applyPending(app);
    consumePendingEconomy(app);  // 经济定位线程的结构化结果由本线程统一落地
    consumePendingCopy(app);
    consumePendingTransferInspection(app);
    consumePendingSplit(app);
    consumePendingExport(app);
    // 刷新写入闸门策略快照（pid / 联运策略）：工作线程只读这份原子快照
    updateWriteGatePolicy(app);
    // 发动机后台（换车/新基准/恢复成败/待恢复记录变化）→ 立即持久化。
    // 不从维护线程碰 ImGui / UI 状态；持久化失败保留修订号差值以便节流重试。
    if (app.engineTuner) {
        const uint64_t revision = app.engineTuner->stateRevision();
        if (revision != app.lastEngineStateRevision &&
            ::GetTickCount64() - app.lastEngineSaveTick >= 1000) {
            app.lastEngineSaveTick = ::GetTickCount64();
            app.engineStateText = app.engineTuner->exportState();
            if (saveSettings(app)) app.lastEngineStateRevision = revision;
        }
    }
    ImGuiIO& io = ::ImGui::GetIO();
    ::ImGui::SetNextWindowPos(ImVec2(0, 0));
    ::ImGui::SetNextWindowSize(io.DisplaySize);
    ::ImGui::Begin("##main", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

    // ---------------- 顶部现代化 Hero Header 状态栏 ----------------
    ::ImGui::BeginChild("##top_hero_panel", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    
    // 主标题与进程状态指示
    {
        const GameId ids[] = {GameId::Ets2, GameId::Ats};
        for (size_t gi = 0; gi < 2; ++gi) {
            const GameDescriptor& d = gameDescriptor(ids[gi]);
            const bool active = (selectedGame() == d.id);
            if (gi > 0) ::ImGui::SameLine();
            if (active) {
                ::ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.95f, 1.0f));
            }
            if (::ImGui::Button(d.shortName, ImVec2(72, 24)) && !active) selectGame(app, d.id);
            if (active) ::ImGui::PopStyleColor();
        }
        if (!supportsVerifiedStructures(selectedGame())) {
            ::ImGui::SameLine();
            ::ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.25f, 1.0f),
                                 "｜美卡布局尚未验证：结构化写入已禁用");
        }
    }
    ::ImGui::Spacing();
    ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "欧卡2 / 美卡");
    ::ImGui::SameLine();
    ::ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.94f, 1.0f), "TRAINER ｜ %s",
                         selectedGameDescriptor().displayName);

    ::ImGui::SameLine(460.0f);
    const bool attached = app.mem.isOpen() && app.mem.alive();
    if (attached) {
        ::ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.50f, 1.0f), "● 游戏已连接运行中");
    } else {
        ::ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "○ 目标游戏未附加");
    }
    ::ImGui::SameLine();
    if (!app.mpHits.empty()) {
        ::ImGui::TextColored(ImVec4(1.00f, 0.35f, 0.35f, 1.0f), " |  ⚠ TruckersMP 联机中");
    } else {
        ::ImGui::TextColored(ImVec4(0.50f, 0.75f, 0.95f, 1.0f),
                             !attached ? " |  联机状态待检测" :
                             (app.mpBlocks ? " |  写入被联机守卫拦截" :
                              (isConvoyCheckEnabled() ? " |  未检测到联机" : " |  未检测 Convoy")));
    }
    if (!isAdmin()) {
        ::ImGui::SameLine();
        ::ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.25f, 1.0f), " [非管理员权限]");
    }

    ::ImGui::Spacing();

    // 快捷按钮条与详情
    if (::ImGui::Button(" 附加游戏 ", ImVec2(105, 28))) attachGame(app);
    ::ImGui::SameLine();
    if (::ImGui::Button(" 刷新检测 ", ImVec2(100, 28))) {
        DWORD pid = 0;
        std::wstring exe;
        if (findProcess(selectedProcessName(), &pid, &exe)) {
            if (!attached) {
                app.procStatus = fmt("发现 %s (PID %u) —— 点「附加游戏」开始", W2U(exe).c_str(), pid);
                logLine(app.procStatus);
            }
        } else {
            app.procStatus = fmt("没有找到 %s，请先启动游戏", W2U(selectedProcessName()).c_str());
            logLine(app.procStatus);
        }
    }
    ::ImGui::SameLine();
    if (::ImGui::Button(" 断开分离 ", ImVec2(90, 28))) detachGame(app);
    ::ImGui::SameLine();
    ::ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "%s", app.procStatus.c_str());
    if (!app.memInfo.empty()) {
        ::ImGui::SameLine();
        ::ImGui::TextColored(ImVec4(0.48f, 0.52f, 0.62f, 1.0f), " (%s)", app.memInfo.c_str());
    }

    ::ImGui::EndChild();
    ::ImGui::Spacing();

    // ---------------- 标签页（独立滚动，避免小屏/高 DPI 下裁切底部操作）----------------
    ::ImGui::BeginChild("##tab_content", ImVec2(0, -145), ImGuiChildFlags_None,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (::ImGui::BeginTabBar("##tabs")) {
        if (::ImGui::BeginTabItem("  实时改钱 / 经验  ")) {
            renderMoneyTab(app);
            ::ImGui::EndTabItem();
        }
        if (::ImGui::BeginTabItem("  车辆 (油量/无损/动力)  ")) {
            renderVehicleTabImpl(app);
            ::ImGui::EndTabItem();
        }
        if (::ImGui::BeginTabItem("  快捷传送 / 自由相机 / 控制台  ")) {
            renderConsoleTabImpl(app);
            ::ImGui::EndTabItem();
        }
        if (::ImGui::BeginTabItem("  全内存扫描器  ")) {
            renderScannerTabImpl(app);
            ::ImGui::EndTabItem();
        }
        if (::ImGui::BeginTabItem("  存档管理 / 备份  ", nullptr, app.activeTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
            app.activeTab = -1;
            renderSaveTabImpl(app);
            ::ImGui::EndTabItem();
        }
        if (::ImGui::BeginTabItem("  说明与设置  ")) {
            renderHelpTabImpl(app);
            ::ImGui::EndTabItem();
        }
        ::ImGui::EndTabBar();
    }
    ::ImGui::EndChild();
    ::ImGui::Spacing();

    // ---------------- 底部运行与日志状态栏 ----------------
    ::ImGui::BeginChild("##bottom_bar", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    
    // 进度条行
    if (app.busy.load()) {
        uint64_t done = app.progressDone.load();
        uint64_t total = app.progressTotal.load();
        float frac = total ? (float)((double)done / (double)total) : 0.0f;
        ::ImGui::ProgressBar(frac, ImVec2(280, 20));
        ::ImGui::SameLine();
        ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "%s  %d%%", app.progressNote.c_str(),
                             (int)(frac * 100.0f + 0.5f));
        ::ImGui::SameLine();
        if (::ImGui::Button(" 取消任务 ", ImVec2(90, 22))) {
            app.cancel.store(true);
            logLine("已请求取消当前任务……");
        }
    } else {
        std::vector<std::string> lines = logSnapshot();
        std::string latest = lines.empty() ? "系统就绪。" : lines.back();
        ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "最新日志：");
        ::ImGui::SameLine();
        ::ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.90f, 1.0f), "%s", latest.c_str());
    }

    ::ImGui::SameLine(io.DisplaySize.x - 220);
    static bool showFullLog = false;
    if (::ImGui::SmallButton(showFullLog ? "隐藏详细日志 ▲" : "查看详细日志 ▼")) {
        showFullLog = !showFullLog;
    }
    ::ImGui::SameLine();
    if (::ImGui::SmallButton("清空日志")) {
        logClear();
    }

    if (!logFileError().empty()) {
        ::ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", logFileError().c_str());
    }
    if (showFullLog) {
        ::ImGui::TextWrapped("实时会话日志：%s", W2U(logFilePath()).c_str());

        ::ImGui::Separator();
        ::ImGui::BeginChild("##log_expanded", ImVec2(0, 56), ImGuiChildFlags_None,
                            ImGuiWindowFlags_HorizontalScrollbar);
        std::vector<std::string> lines = logSnapshot();
        for (const auto& line : lines) {
            ::ImGui::TextUnformatted(line.c_str());
        }
        ::ImGui::SetScrollHereY(1.0f);
        ::ImGui::EndChild();
    }

    ::ImGui::EndChild();
    renderConfirmDialogs(app);
    ::ImGui::End();
}

}  // namespace ets2
