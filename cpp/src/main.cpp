// main.cpp —— 程序入口：Direct3D 11 + Dear ImGui 宿主；支持 --selftest 无界面自检
#include "common.h"
#include "economy.h"
#include "economytest.h"
#include "engine.h"
#include "enginetest.h"
#include "savecopytest.h"
#include "trucktransfertest.h"
#include "profilesplittest.h"
#include "bsiitest.h"
#include "gameio.h"
#include "memory.h"
#include "saves.h"
#include "telemetry.h"
#include "ui.h"
#include "ui_style.h"
#include "gameplay.h"
#include "layout.h"

#include <bcrypt.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

// 后端要求：自己前向声明 Win32 消息处理函数（1.92 的头文件里把它放在 #if 0 里了）
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

using namespace ets2;

namespace {

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT                    g_width = 1280;
UINT                    g_height = 820;
bool                    g_occluded = false;
AppState*               g_app = nullptr;

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

void cleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
        &g_swapChain, &g_device, &featureLevel, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                             D3D11_SDK_VERSION, &desc, &g_swapChain, &g_device,
                                             &featureLevel, &g_context);
    }
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_swapChain) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

void applyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF caption = RGB(17, 20, 22), text = RGB(238, 234, 229);
    // Windows 11 honors these; older Windows versions keep their native caption.
    ::DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    ::DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
}

void applyModernStyle() {
    ui::applyStyle();
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (::ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && g_device && g_swapChain) {
                cleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                           DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
                g_width = (UINT)LOWORD(lParam);
                g_height = (UINT)HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // 屏蔽 Alt 菜单
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 1000;
            info->ptMinTrackSize.y = 650;
            return 0;
        }
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

std::string sha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return out;
    if (::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return out;
    }
    ::BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    uint8_t digest[32] = {0};
    ::BCryptFinishHash(hash, digest, sizeof(digest), 0);
    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    for (uint8_t b : digest) out += fmt("%02x", b);
    return out;
}

}  // namespace

namespace {

int                      g_pass = 0;
int                      g_fail = 0;
std::vector<std::string> g_report;

void check(const std::string& name, bool ok, const std::string& extra = std::string()) {
    if (ok) ++g_pass;
    else ++g_fail;
    std::string line = std::string(ok ? "[OK]   " : "[FAIL] ") + name;
    if (!extra.empty()) line += "  " + extra;
    g_report.push_back(line);
}

void note(const std::string& text) { g_report.push_back("       " + text); }

//: 子进程模式：分配已知数值的缓冲区并把 PID/地址写到文件，等待父进程完成测试
int runTargetHelper(const std::wstring& infoPath) {
    std::vector<uint8_t> buffer(8192, 0);
    uint64_t base = (uint64_t)(uintptr_t)buffer.data();
    *reinterpret_cast<int32_t*>(buffer.data()) = 123456789;
    *reinterpret_cast<int64_t*>(buffer.data() + 8) = 9876543210123LL;
    *reinterpret_cast<float*>(buffer.data() + 16) = 42.5f;
    *reinterpret_cast<double*>(buffer.data() + 24) = 1234.5;
    *reinterpret_cast<float*>(buffer.data() + 32) = 534.75f;  // 假"油量"（显示取整成 534）
    *reinterpret_cast<float*>(buffer.data() + 36) = 0.12f;    // 假"损坏"（12%）
    std::string info = fmt("%u\n%llu\n", (unsigned)::GetCurrentProcessId(),
                           (unsigned long long)base);
    writeFileBytes(infoPath, std::vector<uint8_t>(info.begin(), info.end()));
    std::wstring donePath = infoPath + L".done";
    for (int i = 0; i < 900; ++i) {
        if (::GetFileAttributesW(donePath.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        ::Sleep(100);
    }
    return 0;
}

//: 跨进程端到端测试：启动本程序的"靶子"子进程，对它做完整的 扫描/写入/锁定 验证
int runCrossTest(const std::wstring& outPath) {
    wchar_t exePath[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);
    wchar_t tempDir[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tempDir);
    std::wstring infoPath = std::wstring(tempDir) + L"ets2_trainer_target.txt";
    std::wstring donePath = infoPath + L".done";
    ::DeleteFileW(infoPath.c_str());
    ::DeleteFileW(donePath.c_str());

    std::wstring cmd =
        L"\"" + std::wstring(exePath) + L"\" --target-helper=\"" + infoPath + L"\"";
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                          &pi)) {
        note("启动靶子子进程失败");
        return 1;
    }

    DWORD targetPid = 0;
    uint64_t base = 0;
    for (int i = 0; i < 120; ++i) {
        std::vector<uint8_t> bytes;
        if (readFileBytes(infoPath, bytes) && !bytes.empty()) {
            std::string s(bytes.begin(), bytes.end());
            size_t nl = s.find('\n');
            if (nl != std::string::npos) {
                targetPid = (DWORD)::atoi(s.substr(0, nl).c_str());
                base = (uint64_t)::_strtoui64(s.substr(nl + 1).c_str(), nullptr, 10);
                break;
            }
        }
        ::Sleep(100);
    }
    note("");
    note("=== 跨进程端到端测试（对独立子进程读写）===");
    check("靶子进程已启动", targetPid != 0,
          fmt("(PID %u, 缓冲区 0x%llX)", targetPid, (unsigned long long)base));

    if (targetPid && base) {
        ProcessMemory mem;
        std::string err;
        bool ok = mem.open(targetPid, "target", &err);
        check("打开其它进程", ok, err);
        if (ok) {
            check("跨进程读取 int32", mem.readValue(base, VType::Int32).i == 123456789);
            std::vector<Region> regions = mem.regions(64ull * 1024 * 1024);
            uint64_t total = 0;
            for (const auto& r : regions) total += r.size;
            check("跨进程枚举区域", !regions.empty(),
                  fmt("(%d 个, %s)", (int)regions.size(), formatSize(total).c_str()));

            const ScanOptions opt{64ull * 1024 * 1024, 4, false, false};
            ScanSession s32(mem, VType::Int32);
            uint64_t hits = s32.firstScan(123456789, opt, nullptr, nullptr);
            check("跨进程 int32 扫描", hits >= 1 && s32.contains(base),
                  fmt("(命中 %llu 处)", (unsigned long long)hits));
            check("跨进程写入", mem.writeInt(base, VType::Int32, 987654321) &&
                                    mem.readValue(base, VType::Int32).i == 987654321);

            ScanSession s64(mem, VType::Int64);
            s64.firstScan(9876543210123.0, opt, nullptr, nullptr);
            check("跨进程 int64 扫描", s64.contains(base + 8));

            ScanSession sf(mem, VType::Float, 0, 0.001);
            sf.firstScan(42.5, opt, nullptr, nullptr);
            check("跨进程 float 扫描", sf.contains(base + 16));

            // ---- 油量式扫描：显示值取整 + 容差（534 找到 534.75）----
            ScanSession fuel(mem, VType::Float, 0, 1.0);
            uint64_t fuelHits = fuel.firstScan(534.0, opt, nullptr, nullptr);
            check("油量式容差扫描 (534 命中 534.75)", fuelHits >= 1 && fuel.contains(base + 32),
                  fmt("(命中 %llu 处)", (unsigned long long)fuelHits));
            ScanSession fuelNarrow(mem, VType::Float, 0, 0.1);
            fuelNarrow.firstScan(534.0, opt, nullptr, nullptr);
            check("容差太小时不应命中", !fuelNarrow.contains(base + 32));

            // ---- 损坏式扫描：0.12 -> 锁定为 0 ----
            ScanSession damage(mem, VType::Float, 0, 0.01);
            uint64_t damageHits = damage.firstScan(0.12, opt, nullptr, nullptr);
            check("损坏式容差扫描 (0.12)", damageHits >= 1 && damage.contains(base + 36),
                  fmt("(命中 %llu 处)", (unsigned long long)damageHits));
            size_t writtenDamage = damage.writeAllNumber(0.0);
            check("损坏写入 0（无损）",
                  writtenDamage > 0 && mem.readValue(base + 36, VType::Float).d < 0.0001f);

            Freezer freezer;
            freezer.start(&mem, 100);
            mem.writeInt(base, VType::Int32, 100);
            freezer.add(base, VType::Int32, 55555555.0);
            ::Sleep(400);
            mem.writeInt(base, VType::Int32, 1);
            ::Sleep(400);
            int64_t locked = mem.readValue(base, VType::Int32).i;
            freezer.stop();
            check("跨进程数值锁定", locked == 55555555, fmt("(读到 %lld)", (long long)locked));
        }
    }

    {  // 通知子进程退出
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, donePath.c_str(), L"wb") == 0 && fp) fclose(fp);
    }
    ::WaitForSingleObject(pi.hProcess, 5000);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::DeleteFileW(infoPath.c_str());
    ::DeleteFileW(donePath.c_str());

    std::string summary = fmt("跨进程测试：通过 %d 项，失败 %d 项", g_pass, g_fail);
    g_report.push_back("");
    g_report.push_back(summary);
    std::string text;
    for (const auto& line : g_report) {
        text += line;
        text += "\r\n";
    }
    writeFileBytes(outPath, std::vector<uint8_t>(text.begin(), text.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            std::string msg = summary + "\r\n";
            DWORD written = 0;
            ::WriteConsoleA(h, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
        }
    }
    return g_fail == 0 ? 0 : 1;
}

//: 真实游戏只读实测：只读固定版本指纹、4 级车辆指针链与遥测，不遍历游戏堆。
int runLiveTest(const std::wstring& outPath) {
    g_report.clear();
    const auto& game = selectedGameDescriptor();
    note(fmt("=== %s 只读诊断（PID %u；不申请写入权限） ===", game.shortName, ::GetCurrentProcessId()));
    DWORD pid = 0;
    std::wstring exe;
    const bool found = findProcess(selectedProcessName(), &pid, &exe);
    note(found ? fmt("找到 %s (PID %u)", W2U(exe).c_str(), pid)
               : "目标游戏没有运行");
    // Telemetry is independent of process attachment and remains useful when process access is denied.
    TelemetrySnapshot telemetry;
    std::string error;
    const bool telemetryOk = readTelemetry(&telemetry, &error);
    note(telemetryOk
        ? fmt("遥测通过：game=%u revision=%u fuel=%.2f/%.2f L wear=%.5f/%.5f/%.5f/%.5f/%.5f",
              telemetry.game, telemetry.pluginRevision, telemetry.fuel, telemetry.fuelCapacity,
              telemetry.wear[0], telemetry.wear[1], telemetry.wear[2], telemetry.wear[3], telemetry.wear[4])
        : "遥测不可用：" + error);
    bool opened = false, layoutOk = !game.verifiedLayouts;
    if (found) {
        ProcessMemory memory;
        opened = memory.openReadOnly(pid, W2U(exe), &error);
        note(opened ? "只读进程句柄已打开：禁止写入 API" : "只读附加失败：" + error);
        const auto mp = checkMultiplayer(pid, true);
        note("联机状态：" + mp.detail);
        if (!game.verifiedLayouts) {
            note("固定布局检查跳过：美卡尚未验证，不尝试欧卡偏移。通用扫描/存档/控制台仍可用。");
        } else if (opened && telemetryOk) {
            VehicleLocker vehicle;
            VehicleAddresses addresses;
            layoutOk = vehicle.bind(&memory, pid, &error) && vehicle.probe(&addresses, &telemetry, &error);
            note(layoutOk ? "当前游戏车辆指针及遥测交叉验证通过" : "当前游戏布局校验失败：" + error);
            if (layoutOk) {
                EngineTarget target;
                const bool engineOk = locateEngineTarget(memory, addresses.context, &target, &error);
                note(engineOk ? fmt("发动机只读核对：唯一匹配 %d，扭矩 %.1f N·m，转速 %.0f/%.0f", target.engineMatches, target.torque, target.rpmLimit, target.rpmLimitNeutral) : "发动机核对失败：" + error);
                layoutOk = layoutOk && engineOk;
            }
            if (opened && selectedGame() == GameId::Ats) {
                uint64_t base = 0;
                if (verifyGameLayout(memory, &base, &error)) {
                    ScanSession scan(memory, VType::UInt64, 8);
                    ScanOptions opt;
                    scan.firstScan(static_cast<double>(base + layoutProfile(GameId::Ats).bankVtableRva), opt, nullptr, nullptr);
                    std::vector<uint64_t> fields = scan.addresses();
                    for (auto& address : fields) address += ats_economy_offsets::kMoney;
                    const auto bank = pickBestBankCandidate(memory, fields, 4096);
                    bool economyOk = false;
                    if (bank.unique) {
                        ScanSession refs(memory, VType::UInt64, 8);
                        refs.firstScan(static_cast<double>(bank.object), opt, nullptr, nullptr);
                        const auto economy = pickBestEconomyFromBankRefs(memory, refs.addresses(), bank.object, 4096);
                        economyOk = economy.unique;
                        note(fmt("银行只读核对：金额 %lld；经济对象唯一=%d，经验 %lld", static_cast<long long>(bank.value), economy.unique ? 1 : 0, static_cast<long long>(economy.value)));
                    } else note("银行对象不是唯一有效候选，拒绝自动写入");
                    layoutOk = layoutOk && bank.unique && economyOk;
                } else layoutOk = false;
            }
        }
    }
    const bool ok = found && opened && telemetryOk && layoutOk;
    note(ok ? "只读诊断通过；未验证任何写入功能。" : "只读诊断未通过，请按上述具体原因处理。");
    note("全程未向游戏或遥测共享内存写入。");
    std::string report;
    for (const auto& line : g_report) report += line + "\r\n";
    const bool saved = writeFileBytes(outPath, {report.begin(), report.end()});
    return saved && ok ? 0 : 1;
}

//: 发动机 / 变速箱数据只读定位诊断：有界指针遍历 + 字段签名校验，全程不写入。
int runEngineProbeTest(const std::wstring& outPath) {
    g_report.clear();
    note("=== 发动机数据只读定位（有界指针遍历，不扫描内存） ===");
    DWORD pid = 0;
    std::wstring exe;
    if (!findProcess(selectedProcessName(), &pid, &exe)) {
        note("游戏没在运行（请先启动游戏并进入驾驶界面）");
    } else {
        note(fmt("找到 %s (PID %u)", W2U(exe).c_str(), pid));
        {
            MultiplayerStatus mp = checkMultiplayer(pid, true);
            note("联机检测：" + mp.detail +
                 (mp.blocksWrite() ? "（写入会被拒绝）" : "（允许写入）"));
        }
        ProcessMemory mem;
        std::string error;
        if (!mem.open(pid, W2U(exe), &error)) {
            note("附加失败: " + error);
        } else {
            VehicleLocker vehicle;
            VehicleAddresses addresses;
            TelemetrySnapshot telemetry;
            if (!vehicle.bind(&mem, pid, &error)) {
                note("版本指纹未通过: " + error);
            } else if (!vehicle.probe(&addresses, &telemetry, &error)) {
                note("车辆固定指针链未通过安全校验: " + error);
            } else {
                note(fmt("车辆对象 truck=%s / context=%s / root=%s",
                         hexAddr(addresses.truck).c_str(), hexAddr(addresses.context).c_str(),
                         hexAddr(addresses.rootGlobal).c_str()));
                EngineTarget target;
                if (locateEngineTarget(mem, addresses.context, &target, &error)) {
                    note(fmt("[发动机数据] container=%s engineData=%s 签名命中=%d",
                             hexAddr(target.container).c_str(), hexAddr(target.engineData).c_str(),
                             target.engineMatches));
                    note(fmt("  torque=%.1f N·m  resistance=%.1f  rpmIdle=%.0f  rpmLimit=%.0f  "
                             "rpmLimitNeutral=%.0f",
                             target.torque, target.resistanceTorque, target.rpmIdle,
                             target.rpmLimit, target.rpmLimitNeutral));
                    if (target.transmissionData) {
                        note(fmt("  变速箱数据=%s  differential=%.3f  stall=%.3f",
                                 hexAddr(target.transmissionData).c_str(), target.differentialRatio,
                                 target.stallTorqueRatio));
                    }
                    for (const auto& item : target.layout) {
                        note(fmt("  f+0x%llX = %.6g", (unsigned long long)item.first,
                                 (double)item.second));
                    }
                } else {
                    note("发动机数据定位失败: " + error);
                }
                AccessoryProbeResult probe;
                std::vector<std::pair<std::string, uint64_t>> roots = {
                    {"truck", addresses.truck},
                    {"context", addresses.context},
                    {"root", addresses.rootGlobal},
                };
                uint64_t started = ::GetTickCount64();
                if (!probeAccessories(mem, roots, &probe, &error)) {
                    note("探针失败: " + error);
                } else {
                    note(fmt("探针完成（%llu 毫秒）：%s", (unsigned long long)(::GetTickCount64() - started),
                             probe.note.c_str()));
                    for (const auto& node : probe.named) {
                        note(fmt("[具名对象] %s -> %s (深度 %d)", hexAddr(node.object).c_str(),
                                 node.path.c_str(), node.depth));
                        for (const auto& item : node.strings) {
                            note(fmt("  str+0x%llX = %s", (unsigned long long)item.first,
                                     item.second.c_str()));
                        }
                        int printed = 0;
                        for (const auto& item : node.floats) {
                            if (item.first < 0x1C0 || item.first > 0x2A0) continue;
                            if (printed++ >= 48) break;
                            note(fmt("  f+0x%llX = %.6g", (unsigned long long)item.first,
                                     (double)item.second));
                        }
                        printed = 0;
                        for (const auto& item : node.pointers) {
                            if (printed++ >= 12) break;
                            note(fmt("  p+0x%llX = %s", (unsigned long long)item.first,
                                     hexAddr(item.second).c_str()));
                        }
                    }
                    for (const auto& hit : probe.engines) {
                        note(fmt("[发动机候选] %s -> %s", hexAddr(hit.object).c_str(),
                                 hit.path.c_str()));
                        note(fmt("  torque=%.1f N·m  power=%.1f  rpmIdle=%.0f  rpmLimit=%.0f  "
                                 "rpmLow=%.0f  rpmHigh=%.0f",
                                 hit.torque, hit.power, hit.rpmIdle, hit.rpmLimit,
                                 hit.rpmRangeLow, hit.rpmRangeHigh));
                        for (const auto& item : hit.strings) {
                            note(fmt("  str+0x%llX = %s", (unsigned long long)item.first,
                                     item.second.c_str()));
                        }
                        for (const auto& item : hit.floats) {
                            note(fmt("  f+0x%llX = %.6g", (unsigned long long)item.first,
                                     (double)item.second));
                        }
                        for (const auto& item : hit.pointers) {
                            note(fmt("  p+0x%llX = %s", (unsigned long long)item.first,
                                     hexAddr(item.second).c_str()));
                        }
                    }
                    for (const auto& hit : probe.transmissions) {
                        note(fmt("[变速箱候选] %s -> %s", hexAddr(hit.object).c_str(),
                                 hit.path.c_str()));
                        note(fmt("  stall_torque_ratio=%.3f  differential_ratio=%.3f",
                                 hit.stallTorqueRatio, hit.differentialRatio));
                        for (const auto& item : hit.strings) {
                            note(fmt("  str+0x%llX = %s", (unsigned long long)item.first,
                                     item.second.c_str()));
                        }
                        for (const auto& item : hit.floats) {
                            note(fmt("  f+0x%llX = %.6g", (unsigned long long)item.first,
                                     (double)item.second));
                        }
                        for (const auto& item : hit.pointers) {
                            note(fmt("  p+0x%llX = %s", (unsigned long long)item.first,
                                     hexAddr(item.second).c_str()));
                        }
                        int familyLines = 0;
                        for (const auto& entry : hit.family) {
                            bool interesting = !entry.name.empty();
                            for (const auto& num : entry.nums) {
                                if (num.second >= 1000.0f && num.second <= 8000.0f) {
                                    interesting = true;
                                }
                            }
                            if (!interesting) continue;
                            if (familyLines++ >= 60) break;
                            note(fmt("  [兄弟槽] 0x%llX -> %s  %s",
                                     (unsigned long long)entry.slot, hexAddr(entry.target).c_str(),
                                     entry.name.c_str()));
                            int numLines = 0;
                            for (const auto& num : entry.nums) {
                                if (numLines++ >= 24) break;
                                note(fmt("      f+0x%llX = %.6g", (unsigned long long)num.first,
                                         (double)num.second));
                            }
                        }
                    }
                }
            }
            note("全程只读，没有写入任何数据。");
        }
    }
    std::string text;
    for (const auto& line : g_report) {
        text += line;
        text += "\r\n";
    }
    writeFileBytes(outPath, std::vector<uint8_t>(text.begin(), text.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            for (const auto& line : g_report) {
                std::string msg = line + "\r\n";
                DWORD written = 0;
                ::WriteConsoleA(h, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
            }
        }
    }
    return 0;
}

// ---------- 动力验证用的只读小工具 ----------
bool readFloatAt(const ProcessMemory& mem, uint64_t address, float* out) {
    float value = 0.0f;
    if (!mem.read(address, &value, sizeof(value)) || !std::isfinite(value)) return false;
    *out = value;
    return true;
}

// 只读取附属总成容器里各个对象的 0x180..0x2D0 浮点，用于前后对比。
std::map<uint64_t, float> snapshotContainer(const ProcessMemory& mem, uint64_t container) {
    std::map<uint64_t, float> values;
    std::vector<uint8_t> window(0x1000, 0);
    size_t got = 0;
    if (!mem.read(container, window.data(), window.size(), &got) || got < 16) return values;
    for (size_t offset = 0; offset + 8 <= got; offset += 8) {
        uint64_t pointer = 0;
        std::memcpy(&pointer, window.data() + offset, sizeof(pointer));
        if (pointer < 0x10000ull || pointer > 0x00007FFFFFFFFFFFull) continue;
        std::vector<uint8_t> object(0x150, 0);
        size_t objectGot = 0;
        if (!mem.read(pointer + 0x180, object.data(), object.size(), &objectGot)) continue;
        for (size_t i = 0; i + 4 <= objectGot; i += 4) {
            float value = 0.0f;
            std::memcpy(&value, object.data() + i, sizeof(value));
            if (!std::isfinite(value) || std::fabs(value) < 1e-6f ||
                std::fabs(value) > 1e7f) {
                continue;
            }
            values[pointer + 0x180 + i] = value;
        }
    }
    return values;
}

void reportFloatDiff(const std::map<uint64_t, float>& before,
                     const std::map<uint64_t, float>& after, const char* label) {
    int changed = 0;
    for (const auto& item : after) {
        auto it = before.find(item.first);
        if (it == before.end()) continue;
        if (std::fabs(it->second - item.second) <=
            (std::max)(0.5f, std::fabs(it->second) * 0.01f)) {
            continue;
        }
        if (changed++ >= 40) break;
        note(fmt("  [%s变化] %s: %.6g -> %.6g", label, hexAddr(item.first).c_str(),
                 (double)it->second, (double)item.second));
    }
    note(fmt("  %s共 %d 处数值变化", label, changed));
}

//: 动力调节验证：临时写入 1.25× 扭矩，观察数值是否保持、是否有派生字段随之变化，然后立即恢复。
int runPowerTest(const std::wstring& outPath) {
    g_report.clear();
    note("=== 发动机动力调节验证（写入后立即恢复原值） ===");
    DWORD pid = 0;
    std::wstring exe;
    if (!findProcess(selectedProcessName(), &pid, &exe)) {
        note("游戏没在运行（请先启动游戏并进入驾驶界面）");
    } else {
        note(fmt("找到 %s (PID %u)", W2U(exe).c_str(), pid));
        {
            MultiplayerStatus mp = checkMultiplayer(pid, true);
            note("联机检测：" + mp.detail +
                 (mp.blocksWrite() ? "（写入会被拒绝）" : "（允许写入）"));
        }
        ProcessMemory mem;
        std::string error;
        if (!mem.open(pid, W2U(exe), &error)) {
            note("附加失败: " + error);
        } else if (checkMultiplayer(pid, true).blocksWrite()) {
            note("联机环境不执行写入测试。");
        } else {
            VehicleLocker probe;
            VehicleAddresses addresses;
            TelemetrySnapshot telemetry;
            if (!probe.bind(&mem, pid, &error)) {
                note("版本指纹未通过: " + error);
            } else if (!probe.probe(&addresses, &telemetry, &error)) {
                note("车辆固定指针链未通过安全校验: " + error);
            } else {
                EngineTarget target;
                if (!locateEngineTarget(mem, addresses.context, &target, &error)) {
                    note("发动机数据定位失败: " + error);
                } else {
                    note(fmt("目标：container=%s engineData=%s（签名命中 %d）",
                             hexAddr(target.container).c_str(), hexAddr(target.engineData).c_str(),
                             target.engineMatches));
                    note(fmt("原值：torque=%.1f N·m，rpmLimit=%.0f，rpmLimitNeutral=%.0f",
                             target.torque, target.rpmLimit, target.rpmLimitNeutral));
                    const std::map<uint64_t, float> baseline = snapshotContainer(mem, target.container);
                    note(fmt("容器快照：%d 个浮点值", (int)baseline.size()));

                    EngineTuner tuner;
                    tuner.setBackgroundThreadEnabled(false);  // 诊断模式使用同步流程
                    tuner.attach(std::make_unique<ProcessMemoryAdapter>(&mem),
                                 std::make_unique<GameEngineResolver>(&mem, pid), nullptr);
                    if (!tuner.apply(1.25f, false, &error)) {
                        note("写入失败: " + error);
                    } else {
                        note("已写入 1.25×：" + tuner.status());
                        float live = 0.0f;
                        ::Sleep(1500);
                        readFloatAt(mem, target.engineData + engine_runtime::kTorque, &live);
                        note(fmt("1.5 秒后读回 torque=%.1f（期望 %.1f）", live,
                                 target.torque * 1.25f));
                        const std::map<uint64_t, float> after = snapshotContainer(mem, target.container);
                        reportFloatDiff(baseline, after, "写入后");

                        std::string restoreError;
                        const bool restoredOk = tuner.release(&restoreError);
                        note(restoredOk ? "已写回原值" : ("写回失败: " + restoreError));
                        ::Sleep(800);
                        float restored = 0.0f;
                        readFloatAt(mem, target.engineData + engine_runtime::kTorque, &restored);
                        note(fmt("恢复后读回 torque=%.1f（期望 %.1f）", restored, target.torque));
                        const std::map<uint64_t, float> back = snapshotContainer(mem, target.container);
                        reportFloatDiff(baseline, back, "恢复后");
                    }
                    tuner.restoreAndDetach();
                }
            }
        }
    }
    std::string text;
    for (const auto& line : g_report) {
        text += line;
        text += "\r\n";
    }
    writeFileBytes(outPath, std::vector<uint8_t>(text.begin(), text.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            for (const auto& line : g_report) {
                std::string msg = line + "\r\n";
                DWORD written = 0;
                ::WriteConsoleA(h, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
            }
        }
    }
    return 0;
}

//: 附加字段只读探查：变速箱传动比数组与发动机曲线数组是否可达（全程只读）。
bool readTelemetryRaw(std::vector<uint8_t>* out, std::string* error) {
    HANDLE handle = ::OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\SCSTelemetry");
    if (!handle) {
        if (error) *error = "OpenFileMapping(Local\\SCSTelemetry) 失败";
        return false;
    }
    const void* view = ::MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        ::CloseHandle(handle);
        if (error) *error = "MapViewOfFile 失败";
        return false;
    }
    out->assign((const uint8_t*)view, (const uint8_t*)view + 32768);
    ::UnmapViewOfFile(view);
    ::CloseHandle(handle);
    return true;
}

// 在 [base, base+span) 里按 4 字节步进查找期望的取值序列。
bool findFloatSequence(const ProcessMemory& mem, uint64_t base, uint64_t span,
                       const std::vector<float>& expected, uint64_t* hitOffset) {
    std::vector<uint8_t> window((size_t)span, 0);
    size_t got = 0;
    if (!mem.read(base, window.data(), window.size(), &got) || got < span) return false;
    for (size_t offset = 0; offset + expected.size() * 4 <= got; offset += 4) {
        bool match = true;
        for (size_t i = 0; i < expected.size(); ++i) {
            float value = 0.0f;
            std::memcpy(&value, window.data() + offset + i * 4, sizeof(value));
            const float tolerance = (std::max)(0.02f, std::fabs(expected[i]) * 0.005f);
            if (!std::isfinite(value) || std::fabs(value - expected[i]) > tolerance) {
                match = false;
                break;
            }
        }
        if (match) {
            *hitOffset = base + offset;
            return true;
        }
    }
    return false;
}

int runFieldProbe(const std::wstring& outPath) {
    g_report.clear();
    note("=== 附加字段只读探查（变速箱传动比 / 发动机曲线）===");
    DWORD pid = 0;
    std::wstring exe;
    if (!findProcess(selectedProcessName(), &pid, &exe)) {
        note("游戏没在运行");
    } else {
        ProcessMemory mem;
        std::string error;
        if (!mem.open(pid, W2U(exe), &error)) {
            note("附加失败: " + error);
        } else {
            VehicleLocker vehicle;
            VehicleAddresses addresses;
            TelemetrySnapshot telemetry;
            EngineTarget target;
            if (!vehicle.bind(&mem, pid, &error)) {
                note("版本指纹未通过: " + error);
            } else if (!vehicle.probe(&addresses, &telemetry, &error)) {
                note("车辆指针链未通过: " + error);
            } else if (!locateEngineTarget(mem, addresses.context, &target, &error)) {
                note("发动机数据定位失败: " + error);
            } else {
                note(fmt("truck=%s engineData=%s transmission=%s",
                         hexAddr(addresses.truck).c_str(), hexAddr(target.engineData).c_str(),
                         hexAddr(target.transmissionData).c_str()));

                // 1) 遥测共享内存里的疑似前进挡传动比（只读）
                std::vector<uint8_t> raw;
                std::string rawError;
                std::vector<float> ratios;
                if (readTelemetryRaw(&raw, &rawError)) {
                    for (int i = 0; i < 12; ++i) {
                        float value = 0.0f;
                        ::memcpy(&value, raw.data() + 816 + i * 4, sizeof(value));
                        ratios.push_back(value);
                    }
                    std::string text;
                    for (float value : ratios) text += fmt("%.2f ", (double)value);
                    note("遥测 816 起 12 个值: " + text);
                } else {
                    note("遥测原始读取失败: " + rawError);
                }

                // 2) 变速箱数据对象里是否存在指向传动比数组的指针
                std::vector<std::pair<std::string, std::vector<float>>> sequences;
                if (ratios.size() >= 3) {
                    sequences.push_back({"前进挡", std::vector<float>(ratios.begin(), ratios.begin() + 3)});
                }
                if (raw.size() >= 928) {
                    std::vector<float> reverse;
                    for (int i = 0; i < 3; ++i) {
                        float value = 0.0f;
                        ::memcpy(&value, raw.data() + 912 + i * 4, sizeof(value));
                        reverse.push_back(value);
                    }
                    if (std::fabs(reverse[0]) > 0.5f) sequences.push_back({"倒挡", reverse});
                }
                if (target.transmissionData) {
                    std::vector<uint8_t> window(0x400, 0);
                    size_t got = 0;
                    if (mem.read(target.transmissionData, window.data(), window.size(), &got)) {
                        for (const auto& sequence : sequences) {
                            int found = 0;
                            for (size_t offset = 0; offset + 8 <= got; offset += 8) {
                                uint64_t pointer = 0;
                                ::memcpy(&pointer, window.data() + offset, sizeof(pointer));
                                if (pointer < 0x10000ull || pointer > 0x00007FFFFFFFFFFFull) continue;
                                uint64_t hit = 0;
                                if (!findFloatSequence(mem, pointer, 512, sequence.second, &hit)) continue;
                                std::vector<uint8_t> head(64, 0);
                                size_t headGot = 0;
                                std::string text;
                                if (mem.read(hit, head.data(), head.size(), &headGot)) {
                                    for (int i = 0; i < 8; ++i) {
                                        float value = 0.0f;
                                        ::memcpy(&value, head.data() + i * 4, sizeof(value));
                                        text += fmt("%.2f ", (double)value);
                                    }
                                }
                                note(fmt("  %s传动比数组命中: 槽+0x%llX -> %s [%s]",
                                         sequence.first.c_str(), (unsigned long long)offset,
                                         hexAddr(hit).c_str(), text.c_str()));
                                if (++found >= 4) break;
                            }
                            if (!found) {
                                note(fmt("  变速箱对象内未找到 %s 传动比数组",
                                         sequence.first.c_str()));
                            }
                        }
                    }
                }

                // 3) 发动机曲线：在发动机对象指针里找“递增转速 + 扭矩”型数组
                int curves = 0;
                std::vector<uint8_t> window(0x400, 0);
                size_t got = 0;
                if (mem.read(target.engineData, window.data(), window.size(), &got)) {
                    for (size_t offset = 0; offset + 8 <= got; offset += 8) {
                        uint64_t pointer = 0;
                        ::memcpy(&pointer, window.data() + offset, sizeof(pointer));
                        if (pointer < 0x10000ull || pointer > 0x00007FFFFFFFFFFFull) continue;
                        std::vector<uint8_t> head(192, 0);
                        size_t headGot = 0;
                        if (!mem.read(pointer, head.data(), head.size(), &headGot) || headGot < 64) continue;
                        bool ordered = true;
                        float previous = -1.0f;
                        std::string text;
                        int ascending = 0;
                        for (int i = 0; i < 8; ++i) {
                            float value = 0.0f;
                            ::memcpy(&value, head.data() + i * 4, sizeof(value));
                            text += fmt("%.4g ", (double)value);
                            if (value > previous) ++ascending;
                            previous = value;
                        }
                        if (ascending >= 6) {
                            note(fmt("  疑似曲线数组: 槽+0x%llX -> %s [%s]",
                                     (unsigned long long)offset, hexAddr(pointer).c_str(),
                                     text.c_str()));
                            if (++curves >= 6) break;
                        }
                    }
                    if (!curves) note("  发动机对象内未找到明显递增的曲线数组");
                }
            }
            note("全程只读，没有写入任何数据。");
        }
    }
    std::string text;
    for (const auto& line : g_report) {
        text += line;
        text += "\r\n";
    }
    writeFileBytes(outPath, std::vector<uint8_t>(text.begin(), text.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            for (const auto& line : g_report) {
                std::string msg = line + "\r\n";
                DWORD written = 0;
                ::WriteConsoleA(h, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
            }
        }
    }
    return 0;
}

//: 飞行模式只读检查：config.cfg 开关状态 + controls.sii 相机按键 + 游戏是否在运行。
int runFlyCheck(const std::wstring& outPath) {
    g_report.clear();
    note("=== 飞行模式只读检查 ===");
    std::string text, error;
    if (readGameConfigText(&text, &error)) {
        std::string value;
        const bool dev = readConfigKey(text, "g_developer", &value);
        const std::string developer = dev ? value : std::string("(缺)");
        const bool con = readConfigKey(text, "g_console", &value);
        const std::string console = con ? value : std::string("(缺)");
        const bool speed = readConfigKey(text, "g_flyspeed", &value);
        const std::string flyspeed = speed ? value : std::string("(缺)");
        note(fmt("config.cfg：g_developer=%s  g_console=%s  g_flyspeed=%s", developer.c_str(),
                 console.c_str(), flyspeed.c_str()));
    } else {
        note(error);
    }
    std::vector<SaveSlot> slots = listSlots();
    if (slots.empty()) {
        note("没有找到 profile，无法读取 controls.sii");
    } else {
        CameraBindings bindings;
        if (cameraKeyBindings(slots.front().profileDir + L"\\controls.sii", &bindings)) {
            note(fmt("相机按键：开关 0x%02X、前进 0x%02X、后退 0x%02X、左 0x%02X、右 0x%02X、"
                     "上升 0x%02X、下降 0x%02X",
                     bindings.toggle, bindings.forward, bindings.back, bindings.left,
                     bindings.right, bindings.up, bindings.down));
        } else {
            note("controls.sii 里没有可识别的 mix cam* 绑定");
        }
    }
    DWORD pid = 0;
    std::wstring exe;
    if (findProcess(selectedProcessName(), &pid, &exe)) {
        note("游戏正在运行：写入 config.cfg 会被游戏退出时覆盖，请先完全退出游戏。");
    } else {
        note("游戏未运行：现在可以安全写入 config.cfg。");
    }
    note("全程只读，没有写入任何数据。");
    std::string report;
    for (const auto& line : g_report) {
        report += line;
        report += "\r\n";
    }
    writeFileBytes(outPath, std::vector<uint8_t>(report.begin(), report.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteConsoleA(h, report.c_str(), (DWORD)report.size(), &written, nullptr);
        }
    }
    return 0;
}

//: 地图传送城市列表只读检查：解密存档并提取 goto 可用的城市内部名。
int runCityCheck(const std::wstring& outPath) {
    g_report.clear();
    note("=== 地图传送：城市列表（只读，会解密存档）===");
    std::vector<SaveSlot> slots = listSlots();
    if (slots.empty()) {
        note("没有找到存档");
    } else {
        const SaveSlot& slot = slots.front();
        note(fmt("存档：%s（%s）", slot.label.c_str(), slot.formatText.c_str()));
        std::vector<std::string> cities;
        std::string error;
        const uint64_t started = ::GetTickCount64();
        if (listMapCities(slot.gameSii, &cities, &error)) {
            note(fmt("提取到 %d 个城市内部名，耗时 %llu 毫秒", (int)cities.size(),
                     (unsigned long long)(::GetTickCount64() - started)));
            std::string line;
            for (size_t i = 0; i < cities.size(); ++i) {
                line += cities[i];
                line += " ";
                if ((i + 1) % 10 == 0) {
                    note(line);
                    line.clear();
                }
            }
            if (!line.empty()) note(line);
        } else {
            note("提取失败：" + error);
        }
    }
    note("全程只读，没有写入任何数据。");
    std::string report;
    for (const auto& line : g_report) {
        report += line;
        report += "\r\n";
    }
    writeFileBytes(outPath, std::vector<uint8_t>(report.begin(), report.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteConsoleA(h, report.c_str(), (DWORD)report.size(), &written, nullptr);
        }
    }
    return 0;
}

//: 联运（Convoy）状态只读检查：联机判定 + 会话时间 + 在线玩家列表。
int runConvoyCheck(const std::wstring& outPath) {
    g_report.clear();
    note("=== 联运模式只读检查 ===");
    DWORD pid = 0;
    std::wstring exe;
    const bool running = findProcess(selectedProcessName(), &pid, &exe);
    MultiplayerStatus status = checkMultiplayer(pid, true);
    note("联机判定：" + status.detail +
         (status.blocksWrite() ? "（写入会被拒绝）" : "（允许写入）"));
    note(running ? fmt("游戏正在运行（PID %u）", pid) : "游戏未运行（下面读的是上一次的日志）");
    ConvoySession session = readConvoySession();
    note(fmt("Convoy 会话：%s", session.active ? "进行中" : "未进行"));
    if (session.active) {
        note(fmt("开始时间：%s，在线 %d 人", session.startedAt.c_str(),
                 (int)session.players.size()));
        for (const auto& player : session.players) {
            note(fmt("  - %s (client %d)%s", player.name.c_str(), player.clientId,
                     player.you ? "  ← 你" : ""));
        }
    } else {
        note(session.note);
    }
    note("全程只读，没有写入任何数据。");
    std::string report;
    for (const auto& line : g_report) {
        report += line;
        report += "\r\n";
    }
    writeFileBytes(outPath, std::vector<uint8_t>(report.begin(), report.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteConsoleA(h, report.c_str(), (DWORD)report.size(), &written, nullptr);
        }
    }
    return 0;
}

//: 车辆一键定位的只读诊断：遥测取值 + 精确浮点扫描（绝不写入）
int runAutoLocateTest(const std::wstring& outPath) {
#if 0  // 已停用：全堆扫描会显著增加运行中游戏的内存压力。
    g_report.clear();
    note("=== 车辆数值自动定位（只读） ===");
    TelemetrySnapshot telemetry;
    std::string telemetryError;
    if (!readTelemetry(&telemetry, &telemetryError)) {
        note("遥测读取失败: " + telemetryError);
    } else {
        note(fmt("遥测 revision=%u / ETS2 %u.%u / fuel=%.7g / capacity=%.7g",
                 telemetry.pluginRevision, telemetry.gameMajor, telemetry.gameMinor,
                 telemetry.fuel, telemetry.fuelCapacity));
        note(fmt("损伤 E=%.7g T=%.7g C=%.7g H=%.7g W=%.7g", telemetry.wear[0],
                 telemetry.wear[1], telemetry.wear[2], telemetry.wear[3], telemetry.wear[4]));
        DWORD pid = 0;
        std::wstring exe;
        if (!findProcess(selectedProcessName(), &pid, &exe)) {
            note("游戏进程未运行");
        } else {
            enableDebugPrivilege();
            ProcessMemory mem;
            std::string error;
            if (!mem.open(pid, W2U(exe), &error)) {
                note("附加失败: " + error);
            } else {
                const char* names[] = {"fuel", "wearEngine", "wearTransmission", "wearCabin",
                                       "wearChassis", "wearWheels"};
                float values[] = {telemetry.fuel, telemetry.wear[0], telemetry.wear[1],
                                  telemetry.wear[2], telemetry.wear[3], telemetry.wear[4]};
                std::array<std::vector<uint64_t>, 6> allAddresses;
                ScanOptions options;
                options.workers = 8;
                for (int i = 0; i < 6; ++i) {
                    // 遥测给的是游戏使用的原始 float，不需要按界面显示值做模糊匹配。
                    ScanSession scan(mem, VType::Float, 4, 0.0);
                    uint64_t started = ::GetTickCount64();
                    uint64_t hits = scan.firstScan(values[i], options, nullptr, nullptr);
                    note(fmt("%s %.9g -> %llu 处 / %.2f 秒", names[i], values[i],
                             (unsigned long long)hits,
                             (::GetTickCount64() - started) / 1000.0));
                    const auto& addresses = scan.addresses();
                    allAddresses[(size_t)i] = addresses;
                    size_t shown = (std::min)(addresses.size(), (size_t)20);
                    for (size_t k = 0; k < shown; ++k) note("  " + hexAddr(addresses[k]));
                    if (addresses.size() > shown) note("  ...");
                }
                std::array<std::unordered_set<uint64_t>, 5> wearSets;
                for (size_t i = 0; i < wearSets.size(); ++i) {
                    wearSets[i].insert(allAddresses[i + 1].begin(), allAddresses[i + 1].end());
                }
                std::vector<uint64_t> sequences;
                for (uint64_t address : allAddresses[1]) {
                    bool match = true;
                    for (size_t i = 1; i < wearSets.size(); ++i) {
                        if (!wearSets[i].count(address + i * sizeof(float))) {
                            match = false;
                            break;
                        }
                    }
                    if (match) sequences.push_back(address);
                }
                note(fmt("五项损伤连续结构 -> %llu 处", (unsigned long long)sequences.size()));
                for (uint64_t address : sequences) note("  " + hexAddr(address));
                for (auto& addresses : allAddresses) std::sort(addresses.begin(), addresses.end());
                struct Cluster {
                    uint64_t span;
                    std::array<uint64_t, 5> address;
                };
                std::vector<Cluster> clusters;
                for (uint64_t anchor : allAddresses[1]) {
                    Cluster c{};
                    c.address[0] = anchor;
                    uint64_t lo = anchor, hi = anchor;
                    bool complete = true;
                    for (size_t component = 1; component < 5; ++component) {
                        const auto& candidates = allAddresses[component + 1];
                        auto it = std::lower_bound(candidates.begin(), candidates.end(), anchor);
                        uint64_t best = 0;
                        uint64_t distance = UINT64_MAX;
                        if (it != candidates.end()) {
                            best = *it;
                            distance = best >= anchor ? best - anchor : anchor - best;
                        }
                        if (it != candidates.begin()) {
                            uint64_t previous = *(it - 1);
                            uint64_t d = previous >= anchor ? previous - anchor : anchor - previous;
                            if (d < distance) {
                                best = previous;
                                distance = d;
                            }
                        }
                        if (distance == UINT64_MAX) {
                            complete = false;
                            break;
                        }
                        c.address[component] = best;
                        lo = (std::min)(lo, best);
                        hi = (std::max)(hi, best);
                    }
                    if (complete && hi - lo <= 1024 * 1024) {
                        c.span = hi - lo;
                        clusters.push_back(c);
                    }
                }
                std::sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
                    return a.span < b.span;
                });
                note(fmt("五项损伤同一 1MB 簇 -> %llu 组（列出最紧凑的 30 组）",
                         (unsigned long long)clusters.size()));
                size_t clusterCount = (std::min)(clusters.size(), (size_t)30);
                for (size_t i = 0; i < clusterCount; ++i) {
                    const Cluster& c = clusters[i];
                    note(fmt("  span=0x%llX E=%s T=%s C=%s H=%s W=%s",
                             (unsigned long long)c.span, hexAddr(c.address[0]).c_str(),
                             hexAddr(c.address[1]).c_str(), hexAddr(c.address[2]).c_str(),
                             hexAddr(c.address[3]).c_str(), hexAddr(c.address[4]).c_str()));
                }
                note("全程只读，没有写入任何数据。");
            }
        }
    }
    std::string text;
    for (const auto& line : g_report) text += line + "\r\n";
    writeFileBytes(outPath, std::vector<uint8_t>(text.begin(), text.end()));
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteConsoleA(h, text.c_str(), (DWORD)text.size(), &written, nullptr);
        }
    }
    return 0;
#else
    const std::string text =
        "该测试已停用：车辆功能现在只读取固定版本指纹和动态指针链，不再全堆扫描。\r\n";
    writeFileBytes(outPath, std::vector<uint8_t>(text.begin(), text.end()));
    return 1;
#endif
}

//: 调试用：--parsetest 打印明文存档解析结果到 parse_debug.log
int runParseTest() {
    std::vector<SaveSlot> slots = listSlots();
    const SaveSlot* textSlot = nullptr;
    for (const auto& s : slots) {
        if (s.format == "text") {
            textSlot = &s;
            break;
        }
    }
    if (!textSlot) {
        logLine("没有找到明文存档");
        logSaveToFile(L"parse_debug.log");
        return 1;
    }
    logLine("file=" + W2U(textSlot->gameSii));
    TextValues values;
    std::string err;
    bool ok = readTextValues(textSlot->gameSii, &values, &err);
    logLine(fmt("readTextValues -> ok=%d hasMoney=%d money=%lld hasXp=%d xp=%lld err='%s'",
                (int)ok, (int)values.hasMoney, (long long)values.money, (int)values.hasExperience,
                (long long)values.experience, err.c_str()));
    logSaveToFile(L"parse_debug.log");
    return ok ? 0 : 1;
}

// 自检：无界面；只对自身进程读写，对存档只读（明文测试在临时副本上进行）
// 离线合成回归：不探测真实进程/存档/配置/备份环境，只验证游戏选择与路由契约。
// 用 --gametest 运行；报告写到 exe 同目录的 gametest_report.txt。
int runGameSelftest(const std::wstring& outPath) {
    std::string report;
    int passed = 0, failed = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail) {
        report += fmt("[%s] %s%s\n", ok ? "PASS" : "FAIL", name,
                      detail.empty() ? "" : ("  " + detail).c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
        }
    };

    // 1. 选择 → 进程名 / 遥测编号 / 路径
    setSelectedGame(GameId::Ets2);
    check("默认选择=欧卡2", selectedGame() == GameId::Ets2, "");
    check("欧卡2进程名", std::wstring(selectedProcessName()) == L"eurotrucks2.exe", "");
    check("欧卡2遥测编号=1", selectedGameDescriptor().telemetryGame == 1u, "");
    const std::wstring ets2Docs = documentsDir();
    const std::wstring ets2Backups = backupRoot();
    setSelectedGame(GameId::Ats);
    check("美卡进程名=amtrucks.exe", std::wstring(selectedProcessName()) == L"amtrucks.exe", "");
    check("美卡遥测编号=2", selectedGameDescriptor().telemetryGame == 2u, "");
    check("美卡文档目录不同", documentsDir() != ets2Docs, W2U(documentsDir()));
    check("美卡备份目录隔离", backupRoot() != ets2Backups, W2U(backupRoot()));
    check("美卡有独立的版本布局", supportsVerifiedStructures(GameId::Ats) && layoutProfile(GameId::Ats).rootRva != layoutProfile(GameId::Ets2).rootRva, "");
    const auto& atsProfile = layoutProfile(GameId::Ats);
    check("美卡正确版本指纹通过", layoutFingerprintMatches(atsProfile, atsProfile.timestamp, atsProfile.imageSize, atsProfile.rootInstruction), "");
    auto wrongBytes = atsProfile.rootInstruction; wrongBytes[3] ^= 1;
    check("美卡拒绝被修改的代码", !layoutFingerprintMatches(atsProfile, atsProfile.timestamp, atsProfile.imageSize, wrongBytes), "");
    check("美卡拒绝错误版本", !layoutFingerprintMatches(atsProfile, atsProfile.timestamp+1, atsProfile.imageSize, atsProfile.rootInstruction), "");
    check("美卡拒绝欧卡签名", !layoutFingerprintMatches(atsProfile, atsProfile.timestamp, atsProfile.imageSize, layoutProfile(GameId::Ets2).rootInstruction), "");

    // 2. 结构化写入 fail-closed：动力调校 / 固定油量与无损 / 现金写入
    {
        EngineTuner tuner;
        std::string err;
        check("未附加的美卡动力调校被拒", !tuner.apply(1.25f, false, &err) && !err.empty(), err);
    }
    {
        VehicleLocker locker;
        std::string err1, err2;
        check("未绑定的美卡油量被拒", !locker.setFuelEnabled(true, &err1) && !err1.empty(), err1);
        check("未绑定的美卡无损被拒", !locker.setDamageEnabled(true, &err2) && !err2.empty(), err2);
    }
    {
        ProcessMemory memory;
        std::string err;
        check("无有效美卡进程的现金写入被拒",
              !writeBankMoneyVerified(memory, 0, 0, 1, &err) && !err.empty(), err);
    }

    // ATS fixtures use the observed runtime offsets, not the ETS2 definition table.
    {
        ProcessMemory memory; std::string error; uint64_t base=0;
        const bool opened=memory.openReadOnly(::GetCurrentProcessId(),"fixture",&error) && memory.mainImage(&base);
        std::array<uint8_t,0x80> bank{}, otherBank{};
        std::array<uint8_t,0x800> economy{};
        auto put=[](auto& bytes, size_t offset, const auto& value) { ::memcpy(bytes.data()+offset,&value,sizeof(value)); };
        const uint64_t bankBase=reinterpret_cast<uint64_t>(bank.data()), econBase=reinterpret_cast<uint64_t>(economy.data());
        const uint64_t bankVtable=base+atsProfile.bankVtableRva, econVtable=base+atsProfile.economyVtableRva;
        const int64_t money=28015, fixed=2523, loan=130000;
        const float ratio=0.25234f, severity=1.690996f, seconds=29.0f;
        const uint8_t enabled=1; const int32_t xp=115, minutes=1901;
        put(bank,0,bankVtable); put(bank,0x10,money); put(bank,0x18,fixed); put(bank,0x20,ratio);
        put(bank,0x24,severity); put(bank,0x50,enabled); put(bank,0x58,loan);
        put(economy,0,econVtable); put(economy,0x10,bankBase); put(economy,0x19C,minutes);
        put(economy,0x1A0,seconds); put(economy,0x77C,xp);
        ProbeResult bankProbe, econProbe;
        check("美卡运行银行偏移与整数免赔额",opened && probeBankObject(memory,bankBase+0x10,&bankProbe) && bankProbe.value==28015 && bankProbe.object==bankBase,"");
        check("美卡经济偏移及银行关联",opened && probeEconomyObject(memory,econBase+0x77C,&econProbe) && econProbe.value==115 && economyReferencesBank(memory,econBase,bankBase),"");
        check("美卡事故累计超过 1 仍可定位",probeBankObject(memory,bankBase+0x10,nullptr) && probeEconomyObject(memory,econBase+0x77C,nullptr),"");
        put(bank,0x24,250.0f);
        check("美卡多次事故累计仍可定位",probeBankObject(memory,bankBase+0x10,nullptr),"");
        put(bank,0x24,-0.01f);
        ProbeResult rejected;
        check("美卡负事故值拒绝并说明原因",!probeBankObject(memory,bankBase+0x10,&rejected) && rejected.detail.find("事故累计值")!=std::string::npos,"");
        const uint32_t nonfinite=0x7F800000u;
        put(bank,0x24,nonfinite);
        check("美卡非有限事故值拒绝",!probeBankObject(memory,bankBase+0x10,nullptr),"");
        put(bank,0x24,severity); put(bank,0x20,1.01f);
        check("美卡共保比例仍须不超过 1",!probeBankObject(memory,bankBase+0x10,nullptr),"");
        put(bank,0x20,ratio);
        check("美卡拒绝欧卡金额偏移",!probeBankObject(memory,bankBase+0x18,nullptr),"");
        check("美卡拒绝欧卡经验偏移",!probeEconomyObject(memory,econBase+0x780,nullptr),"");
        auto picked=pickBestBankCandidate(memory,{bankBase+0x10},4);
        check("美卡自动类型扫描唯一候选",picked.unique && picked.value==28015,"");
        otherBank=bank;
        picked=pickBestBankCandidate(memory,{bankBase+0x10,reinterpret_cast<uint64_t>(otherBank.data())+0x10},4);
        check("美卡银行歧义拒绝",!picked.unique && picked.tied==2,"");
        picked=pickBestBankCandidate(memory,{bankBase+0x10,reinterpret_cast<uint64_t>(otherBank.data())+0x10},1);
        check("美卡银行截断拒绝",!picked.unique && picked.truncated,"");
        picked=pickBestEconomyFromBankRefs(memory,{econBase+0x10},bankBase,4);
        check("美卡经济引用自动定位",picked.unique && picked.fieldAddress==econBase+0x77C,"");
        put(economy,0x780,int32_t(7));
        check("美卡技能错位拒绝",!probeEconomyObject(memory,econBase+0x77C,nullptr),"");
        put(economy,0x780,int32_t(0)); put(bank,0,uint64_t(0));
        check("美卡银行对象复用拒绝",!probeBankObject(memory,bankBase+0x10,nullptr) && !probeEconomyObject(memory,econBase+0x77C,nullptr),"");
        put(bank,0,bankVtable);
        check("美卡拒绝写入其他进程",!writeBankMoneyVerified(memory,bankBase+0x10,bankBase,1000000,&error) && *reinterpret_cast<const int64_t*>(bank.data()+0x10)==28015, error);
    }

    {
        AppState fixture;
        fixture.engineStateEts2="ets_credentials";
        fixture.engineStateText="ats_credentials";
        fixture.engineStateAts="stale_ats_credentials";
        const auto text=settingsText(fixture);
        check("双游戏动力凭据分别保存",text.find("engine_state=ets_credentials\n")!=std::string::npos && text.find("engine_state_ats=ats_credentials\n")!=std::string::npos,"");
        const std::wstring path=applicationDir()+L"\\gametest_engine_keys.ini";
        writeFileBytes(path,{text.begin(),text.end()});
        AppState loaded; loadSettingsFrom(loaded,path);
        check("美卡只加载自己的动力凭据",loaded.engineStateText=="ats_credentials" && loaded.engineStateEts2=="ets_credentials","");
        ::DeleteFileW(path.c_str());
    }

    // 3. 跨游戏备份恢复被拒（历史无标签 = 欧卡2；路径边界不被前缀误判）
    {
        BackupInfo legacy;
        legacy.originalDir = ets2Docs + L"\\profiles\\p1";
        BackupInfo atsTagged;
        atsTagged.game = L"ats";
        atsTagged.originalDir = documentsDir() + L"\\profiles\\p1";
        std::string err1, err2, err3;
        check("美卡拒绝历史欧卡2备份", !restoreBackup(legacy, &err1) && !err1.empty(), err1);
        setSelectedGame(GameId::Ets2);
        check("欧卡2拒绝美卡备份", !restoreBackup(atsTagged, &err2) && !err2.empty(), err2);
        BackupInfo wrongPath = atsTagged;
        wrongPath.game = L"ets2";
        wrongPath.originalDir = ets2Docs + L"Other\\profiles\\p1";
        check("来源路径边界不误判", !restoreBackup(wrongPath, &err3) && !err3.empty(), err3);
    }

    // 4. settings：记录当前游戏、忽略历史 anti_roll_factor、收藏点按游戏分区
    {
        AppState app;
        app.teleportSpots.clear();
        setSelectedGame(GameId::Ats);
        const std::string text = settingsText(app);
        check("settings 记录 game=ats", text.find("game=ats") != std::string::npos, "");
        check("settings 不再写 anti_roll", text.find("anti_roll") == std::string::npos, "");
        check("settings 写美卡收藏键", text.find("teleport_spots_ats=") != std::string::npos, "");
        wchar_t tempBuf[MAX_PATH] = {0};
        const DWORD tempLen = ::GetTempPathW(MAX_PATH, tempBuf);
        const std::wstring tmp =
            std::wstring(tempBuf, tempLen) + L"ets2trainer_gametest_settings.ini";
        {
            const std::string seed = "game=ats\nanti_roll_factor=5\nmoney_type=f32\n";
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                ::WriteFile(h, seed.data(), (DWORD)seed.size(), &written, nullptr);
                ::CloseHandle(h);
            }
        }
        AppState loaded;
        const GameId beforeLoad = selectedGame();
        loadSettingsFrom(loaded, tmp);
        check(selectionLockedFromCli() ? "命令行选择不被设置覆盖" : "settings 读取 game=ats",
              selectedGame() == (selectionLockedFromCli() ? beforeLoad : GameId::Ats), "");
        check("settings 忽略历史 anti_roll_factor",
              !loaded.autoFuelLock && !loaded.autoDamageLock, "");
        ::DeleteFileW(tmp.c_str());
    }

    // Exercise the decoder with actual shared-memory bytes; no live mapping is opened.
    {
        std::array<uint8_t, 1056> bytes{};
        bytes[0] = 1;
        const uint32_t revision = 12;
        const float capacity = 500.0f, fuel = 100.0f;
        ::memcpy(bytes.data() + 40, &revision, sizeof(revision));
        ::memcpy(bytes.data() + 704, &capacity, sizeof(capacity));
        ::memcpy(bytes.data() + 1000, &fuel, sizeof(fuel));
        for (GameId game : kAllGameIds) {
            setSelectedGame(game);
            TelemetrySnapshot snapshot;
            std::string error;
            const uint32_t matching = gameDescriptor(game).telemetryGame;
            ::memcpy(bytes.data() + 52, &matching, sizeof(matching));
            check(game == GameId::Ats ? "美卡真实缓冲区匹配" : "欧卡真实缓冲区匹配",
                  decodeTelemetryBuffer(bytes.data(), bytes.size(), &snapshot, &error), error);
            const uint32_t opposite = matching == 1 ? 2 : 1;
            ::memcpy(bytes.data() + 52, &opposite, sizeof(opposite));
            check(game == GameId::Ats ? "美卡拒绝欧卡缓冲区" : "欧卡拒绝美卡缓冲区",
                  !decodeTelemetryBuffer(bytes.data(), bytes.size(), &snapshot, &error) &&
                  error.find("不符") != std::string::npos, error);
        }
    }
    // Switching runs the production cleanup using a dedicated test settings file.
    {
        setSelectedGame(GameId::Ets2);
        AppState app;
        app.slots.emplace_back(); app.backups.emplace_back();
        app.selectedSlot = app.selectedBackup = 0;
        app.slotValues.hasMoney = true;
        ::strcpy_s(app.saveMoney, "123");
        app.cameraLoaded = true;
        app.flyDeveloper = app.flyConsole = 1;
        app.flySpeedSaved = "999";
        app.consoleKey = 'K';
        app.convoy.active = true;
        app.requestRestoreConfirm = app.showBackupDialog = true;
        ::strcpy_s(app.cityInput, "berlin");
        app.locatedMoneyAddress = 0x12345;
        app.teleportSpots.emplace_back("欧洲", "berlin");
        const std::wstring settings = applicationDir() + L"\\gametest_switch_settings.ini";
        check("切换清空旧游戏缓存", selectGame(app, GameId::Ats, settings) &&
              app.slots.empty() && app.backups.empty() && app.selectedSlot == -1 &&
              app.selectedBackup == -1 && !app.slotValues.hasMoney && !app.saveMoney[0] &&
              !app.cameraLoaded && app.flyDeveloper == -1 && app.flyConsole == -1 &&
              app.flySpeedSaved.empty() && app.consoleKey == 0xC0 && !app.convoy.active &&
              !app.requestRestoreConfirm && !app.showBackupDialog && !app.cityInput[0] &&
              !app.locatedMoneyAddress && !app.attached, "");
        app.teleportSpots.emplace_back("美国", "phoenix");
        check("切回欧卡保留欧洲收藏", selectGame(app, GameId::Ets2, settings) &&
              app.teleportSpots.size() == 1 && app.teleportSpots[0].second == "berlin", "");
        check("切回美卡保留美国收藏", selectGame(app, GameId::Ats, settings) &&
              app.teleportSpots.size() == 1 && app.teleportSpots[0].second == "phoenix", "");
        ::DeleteFileW(settings.c_str());
        app.engineStateEts2 = "1;3800;3800;0;2200;2200;4750;4750;0;2200;2200;1.25;0";
        const std::string saved = settingsText(app);
        check("美卡保存保留欧卡恢复凭据", saved.find(app.engineStateEts2) != std::string::npos, "");
        app.engineTuner = std::make_unique<EngineTuner>();
        check("切至欧卡加载欧卡恢复凭据", selectGame(app, GameId::Ets2, settings) &&
              app.engineTuner->hasUnverifiedImportedRecord(), "");
        check("未恢复的发动机记录阻止切换", !selectGame(app, GameId::Ats, settings) &&
              selectedGame() == GameId::Ets2 && !app.engineTuner->exportState().empty(), "");
        ::DeleteFileW(settings.c_str());
    }
    {
        setSelectedGame(GameId::Ets2);
        BackupInfo escaped;
        escaped.originalDir = documentsDir() + L"\\..\\American Truck Simulator\\profiles\\p1";
        std::string error;
        check("备份拒绝点点路径逃逸", !restoreBackup(escaped, &error) &&
              error.find("不属于当前游戏") != std::string::npos, error);
    }
    // A brand-new backup must create its directory before writing the game tag.
    {
        const std::filesystem::path fixture = std::filesystem::path(applicationDir()) /
            (L"gametest_save_" + std::to_wstring(::GetCurrentProcessId()));
        std::error_code ec;
        const bool created = std::filesystem::create_directory(fixture, ec);
        check("创建独立存档测试目录", created && !ec, "");
        if (created && !ec) {
            const std::string text = "SiiNunit\n{\nbank : _nameless.bank {\n money_account: 100\n}\n}\n";
            SaveSlot slot;
            slot.slotDir = fixture.wstring();
            slot.gameSii = (fixture / L"game.sii").wstring();
            slot.profileName = L"gametest";
            slot.slotName = std::to_wstring(::GetCurrentProcessId());
            const bool seeded = writeFileBytes(slot.gameSii, {text.begin(), text.end()});
            check("生成合成存档", seeded, "");
            for (GameId game : kAllGameIds) {
                setSelectedGame(game);
                std::string error;
                const std::wstring dir = seeded ? backupSlot(slot, "gametest", &error) : L"";
                std::vector<uint8_t> tag, copied;
                const auto list = listBackups();
                const auto found = std::find_if(list.begin(), list.end(), [&](const BackupInfo& b) {
                    return b.path == dir && b.game == U2W(gameKey(game));
                });
                const bool ok = !dir.empty() &&
                    readFileBytes(dir + L"\\game.tag", tag) &&
                    std::string(tag.begin(), tag.end()) == std::string("game=") + gameKey(game) + "\n" &&
                    readFileBytes(dir + L"\\files\\game.sii", copied) &&
                    std::string(copied.begin(), copied.end()) == text && found != list.end();
                check(game == GameId::Ats ? "美卡首次备份和标签有效" : "欧卡首次备份和标签有效", ok, error);
                if (!dir.empty() && std::filesystem::path(dir).parent_path() == std::filesystem::path(backupRoot()))
                    std::filesystem::remove_all(std::filesystem::path(dir), ec);
            }
            std::filesystem::remove_all(fixture, ec);
        }
    }
    // Transactions target only the test process's own floats.
    {
        setSelectedGame(GameId::Ets2);
        ProcessMemory memory;
        std::string error;
        const bool opened = memory.open(::GetCurrentProcessId(), "gametest", &error);
        check("车辆离线测试打开自身进程", opened, error);
        if (opened) {
            float fuel = 0.5f, correction = 0.05f, damage = 0.3f, sentinel = 7.0f;
            VehicleAddresses a;
            a.fuel = (uint64_t)(uintptr_t)&fuel;
            a.fuelCorrection = (uint64_t)(uintptr_t)&correction;
            a.damageFields = {(uint64_t)(uintptr_t)&damage};
            VehicleLocker locker;
            locker.bindForTesting(&memory, 0);
            locker.setCommandsForTesting(true, true);
            auto result = locker.runMaintenanceCycle(a, false, &error);
            check("保留油量无损事务", result.action == VehicleLocker::CycleAction::Applied &&
                  fuel == 1.0f && correction == 0.0f && damage == 0.0f && sentinel == 7.0f, error);
            fuel = 0.5f; correction = 0.05f; damage = 0.3f;
            locker.setBeforeFieldWriteHookForTesting([&](size_t i) {
                if (i == 1) locker.setCommandsForTesting(false, false);
            });
            locker.setCommandsForTesting(true, true);
            result = locker.runMaintenanceCycle(a, false, &error);
            check("事务中途取消精确回滚", result.action == VehicleLocker::CycleAction::Failed &&
                  fuel == 0.5f && correction == 0.05f && damage == 0.3f, error);
            locker.setBeforeFieldWriteHookForTesting({});
            locker.setCommandsForTesting(true, true);
            locker.setCycleHookAfterSnapshot([&] { locker.setCommandsForTesting(false, false); });
            result = locker.runMaintenanceCycle(a, false, &error);
            check("过期车辆周期不写入", result.action == VehicleLocker::CycleAction::SkippedStale &&
                  fuel == 0.5f && damage == 0.3f, error);
            locker.setCycleHookAfterSnapshot({});
            locker.setCommandsForTesting(true, true);
            locker.setWriteGuard([] { return std::string("test guard"); });
            locker.setGuardIntervalMs(0);
            result = locker.runMaintenanceCycle(a, false, &error);
            check("车辆写入守卫保留", result.action == VehicleLocker::CycleAction::Blocked &&
                  fuel == 0.5f && damage == 0.3f, error);
            locker.setWriteGuard({});
            setSelectedGame(GameId::Ats);
            locker.setCommandsForTesting(true, true);
            result = locker.runMaintenanceCycle(a, false, &error);
            check("美卡合成油量与无损周期", result.action == VehicleLocker::CycleAction::Applied &&
                  fuel == 1.0f && correction == 0.0f && damage == 0.0f, "");
        }
    }
    // Read-only access remains readable and refuses all writes even to our own buffer.
    {
        ProcessMemory memory;
        float value = 12.5f, changed = 99.0f, read = 0.0f;
        std::string error;
        const uint64_t address = (uint64_t)(uintptr_t)&value;
        const bool opened = memory.openReadOnly(::GetCurrentProcessId(), "readonly-test", &error);
        check("只读句柄可读且不能写", opened && !memory.canWrite() &&
              memory.read(address, &read, sizeof(read)) && read == value &&
              !memory.write(address, &changed, sizeof(changed)) && value == 12.5f, error);
        memory.close();
        check("关闭只读句柄清空状态", !memory.isOpen() && !memory.canWrite(), "");
    }
    // Unattached ATS rejects automatic economy without leaving a pending write intent.
    {
        setSelectedGame(GameId::Ats);
        AppState app;
        app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = true;
        startLocateEconomy(app);
        check("美卡未附加时不扫描且清空写入意图", !app.busy.load() && !app.worker.joinable() &&
              !app.attached && !app.autoWriteMoneyAfterLocate && !app.autoWriteXpAfterLocate &&
              app.locateStatus.find("还没有附加") != std::string::npos, app.locateStatus);
    }
    // Persist each message immediately, serialize concurrent writers, and retain disk evidence on clear.
    {
        const std::wstring path = applicationDir() + fmtW(L"\\gametest_live_%u_%llu.log",
            ::GetCurrentProcessId(), (unsigned long long)::GetTickCount64());
        std::string error;
        const bool started = logStartFile(path, &error);
        check("实时日志创建独立会话文件", started, error);
        if (started) {
            logLine("live-log-first 中文");
            std::vector<uint8_t> bytes;
            const bool immediate = readFileBytes(path, bytes);
            check("日志无需退出即可读取", immediate &&
                  std::string(bytes.begin(), bytes.end()).find("live-log-first 中文") != std::string::npos, "");
            auto writer = [](int n) { for (int i = 0; i < 40; ++i) logLine(fmt("thread-%d-%d", n, i)); };
            std::thread a(writer, 1), b(writer, 2);
            a.join(); b.join();
            logClear();
            logLine("after-clear");
            logStopFile();
            readFileBytes(path, bytes);
            const std::string content(bytes.begin(), bytes.end());
            size_t pos = 0, count = 0;
            while ((pos = content.find("thread-", pos)) != std::string::npos) { ++count; pos += 7; }
            check("并发日志完整且清空不抹除磁盘记录", count == 80 &&
                  content.find("live-log-first 中文") != std::string::npos &&
                  content.find("after-clear") != std::string::npos, "");
            check("拒绝覆盖既有会话日志", !logStartFile(path, &error) &&
                  !logFileError().empty(), error);
            ::DeleteFileW(path.c_str());
        }
    }
    setSelectedGame(GameId::Ets2);
    for (const auto& item : runEngineTunerTests()) check(item.name.c_str(), item.ok, item.detail);
    for (const auto& item : runEconomyLocateTests()) check(item.name.c_str(), item.ok, item.detail);
    for (const auto& item : runSaveCopyTests()) check(item.name.c_str(), item.ok, item.detail);
    for (const auto& item : runReadableExportTests()) check(item.name.c_str(), item.ok, item.detail);
    for (const auto& item : runTruckTransferTests()) check(item.name.c_str(), item.ok, item.detail);
    for (const auto& item : runProfileSplitTests()) check(item.name.c_str(), item.ok, item.detail);
    for (const auto& item : runBsiiTests()) check(item.name.c_str(), item.ok, item.detail);
    setSelectedGame(GameId::Ets2);   // 恢复默认，避免影响后续运行
    report += fmt("共 %d 项通过，%d 项失败\n", passed, failed);
    if (!outPath.empty()) {
        HANDLE h = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteFile(h, report.data(), (DWORD)report.size(), &written, nullptr);
            ::CloseHandle(h);
        }
    }
    ::OutputDebugStringA(report.c_str());
    return (failed == 0) ? 0 : 1;
}

int runSelfTest(const std::wstring& outPath) {
    note("ETS2 Trainer (C++) 自检报告");

    // ---------- 1. 内存引擎 ----------
    note("");
    note("=== 1. 内存引擎（对自身进程读写）===");
    std::vector<uint8_t> buffer(8192, 0);
    uint64_t base = (uint64_t)(uintptr_t)buffer.data();
    *reinterpret_cast<int32_t*>(buffer.data()) = 123456789;
    *reinterpret_cast<int64_t*>(buffer.data() + 8) = 9876543210123LL;
    *reinterpret_cast<float*>(buffer.data() + 16) = 42.5f;
    *reinterpret_cast<double*>(buffer.data() + 24) = 1234.5;

    ProcessMemory mem;
    std::string err;
    const ScanOptions fastOpt{64ull * 1024 * 1024, 4, false, false};
    bool opened = mem.open(::GetCurrentProcessId(), "self", &err);
    check("打开自身进程", opened, err);
    if (opened) {
        check("活进程存活检测（等待句柄具有 SYNCHRONIZE 权限）", mem.alive());
        std::string reuseError;
        check("UI 连续启用复用附加会话，不重置其他功能", validateAttachedSessionReuse(&reuseError), reuseError);
        {
            wchar_t exePath[MAX_PATH * 2]{};
            ::GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);
            const std::wstring eventName = L"Local\\ETS2TrainerLifecycle_" +
                std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(::GetTickCount64());
            HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
            std::wstring childCommand = L"\"" + std::wstring(exePath) +
                L"\" --lifecycle-helper=" + eventName;
            std::vector<wchar_t> mutableCommand(childCommand.begin(), childCommand.end());
            mutableCommand.push_back(0);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            const bool created = event && ::CreateProcessW(nullptr, mutableCommand.data(), nullptr,
                nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
            bool lifecycleOk = false;
            if (created) {
                ProcessMemory childMemory;
                std::string childError;
                const bool aliveBefore = childMemory.open(child.dwProcessId, "exit259-target", &childError) &&
                                         childMemory.alive();
                ::SetEvent(event);
                const bool exited = ::WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0;
                DWORD exitCode = 0;
                lifecycleOk = aliveBefore && exited && ::GetExitCodeProcess(child.hProcess, &exitCode) &&
                              exitCode == 259 && !childMemory.alive();
                ::CloseHandle(child.hProcess);
                ::CloseHandle(child.hThread);
            }
            if (event) ::CloseHandle(event);
            check("进程生命周期：活靶子→退出码 259，退出后仍正确判为死亡", lifecycleOk);
        }
        std::vector<Region> regions = mem.regions(64ull * 1024 * 1024);
        uint64_t total = 0;
        for (const auto& r : regions) total += r.size;
        check("枚举可写内存区域", !regions.empty(),
              fmt("(%d 个区域, %s)", (int)regions.size(), formatSize(total).c_str()));
        check("读取已知 int32", mem.readValue(base, VType::Int32).i == 123456789);

        ScanSession s32(mem, VType::Int32);
        uint64_t hits = s32.firstScan(123456789, fastOpt, nullptr, nullptr);
        check("int32 首次扫描", hits >= 1, fmt("(命中 %llu 处)", (unsigned long long)hits));
        check("命中包含目标地址", s32.contains(base));

        mem.writeInt(base, VType::Int32, 555555);
        uint64_t remain = s32.nextScan(ScanMode::Exact, 555555.0, nullptr, nullptr);
        check("再次扫描(精确) 收敛", remain == 1, fmt("(剩余 %llu 处)", (unsigned long long)remain));
        check("写入并回读 int32", mem.writeInt(base, VType::Int32, 777777) &&
                                      mem.readValue(base, VType::Int32).i == 777777);
        const size_t beforeCancel = s32.count();
        const uint64_t afterCancel =
            s32.nextScan(ScanMode::Changed, 0, nullptr, []() { return true; });
        check("取消收窄保留原候选集", afterCancel == beforeCancel && s32.count() == beforeCancel);

        ScanSession s64(mem, VType::Int64);
        s64.firstScan(9876543210123.0, fastOpt, nullptr, nullptr);
        check("int64 扫描", s64.contains(base + 8));

        ScanSession sf(mem, VType::Float, 0, 0.001);
        sf.firstScan(42.5, fastOpt, nullptr, nullptr);
        check("float 扫描", sf.contains(base + 16));
        mem.writeDouble(base + 16, VType::Float, 88.25);
        uint64_t changed = sf.nextScan(ScanMode::Changed, 0, nullptr, nullptr);
        check("float 再次扫描(变了)", changed >= 1 && sf.contains(base + 16));

        ScanSession sd(mem, VType::Double, 0, 0.001);
        sd.firstScan(1234.5, fastOpt, nullptr, nullptr);
        check("double 扫描", sd.contains(base + 24));

        Freezer freezer;
        freezer.start(&mem, 100);
        mem.writeInt(base, VType::Int32, 100);
        freezer.add(base, VType::Int32, 60606060.0);
        ::Sleep(400);
        mem.writeInt(base, VType::Int32, 1);
        ::Sleep(400);
        int64_t locked = mem.readValue(base, VType::Int32).i;
        freezer.stop();
        check("数值锁定生效", locked == 60606060, fmt("(读到 %lld)", (long long)locked));
        freezer.add(base, VType::Int32, 1.0);
        freezer.start(&mem, 100);
        check("重启锁定器会清除旧地址", freezer.size() == 0);
        freezer.stop();
    }

    // ---------- 2. 进程 / 联机 / 窗口 ----------
    note("");
    note("=== 2. 进程 / 联机检测 / 窗口 ===");
    check("管理员权限检测", true, isAdmin() ? "(当前是管理员)" : "(当前不是管理员)");
    DWORD gamePid = 0;
    std::wstring exe;
    bool gameRunning = findProcess(selectedProcessName(), &gamePid, &exe);
    check("查找欧卡2 进程", true,
          gameRunning ? fmt("(找到 PID %u)", gamePid) : "(游戏没在运行，属正常)");
    if (gameRunning) {
        std::vector<std::wstring> mp = detectMultiplayer(gamePid);
        check("联机(TruckersMP)检测", true, mp.empty() ? "(单机)" : ("(发现 " + W2U(mp.front()) + ")"));
        uintptr_t hwnd = findGameWindow(gamePid);
        check("游戏窗口检测", true,
              hwnd ? fmt("(hwnd=0x%llX)", (unsigned long long)hwnd)
                   : "(进程存在，但当前没有可见主窗口；控制台功能暂不可用)");
    }
    // ---------- 3. 存档解密 ----------
    note("");
    note("=== 3. 存档解密（AES-256-CBC + DEFLATE）===");
    std::vector<SaveSlot> slots = listSlots();
    check("列出存档槽", !slots.empty(), fmt("(%d 个)", (int)slots.size()));
    int scsCount = 0, textCount = 0;
    for (const auto& s : slots) {
        if (s.format == "scs") ++scsCount;
        else if (s.format == "text") ++textCount;
    }
    note(fmt("加密存档 %d 个，明文存档 %d 个", scsCount, textCount));

    const SaveSlot* encrypted = nullptr;
    for (const auto& s : slots) {
        if (s.format == "scs") {
            encrypted = &s;
            break;
        }
    }
    if (encrypted) {
        std::vector<uint8_t> inner;
        DecryptInfo info;
        std::string derr;
        bool ok = decryptSave(encrypted->gameSii, inner, &info, &derr);
        check("ScsC 解密", ok, derr);
        if (ok) {
            // 头部 [52:56] 是"解压后(内层)大小"，见 Python 版实测结论
            check("头部声明长度 == 解压后内层长度", info.declaredSize == info.innerSize,
                  fmt("(%llu vs %llu)", (unsigned long long)info.declaredSize,
                      (unsigned long long)info.innerSize));
            check("zlib 解压成功（内层可识别）",
                  info.innerFormat == "bsii" || info.innerFormat == "text",
                  fmt("(内层 %s, %s)", info.innerFormat.c_str(),
                      formatSize(info.innerSize).c_str()));
            note("内层大小   = " + formatSize(info.innerSize));
            note("内层 SHA-256 = " + sha256Hex(inner));
        }
    } else {
        note("(没有加密存档可测试)");
    }

    // ---------- 4. 明文存档数值 ----------
    note("");
    note("=== 4. 明文存档数值读写（在临时副本上测试）===");
    const SaveSlot* textSlot = nullptr;
    for (const auto& s : slots) {
        if (s.format == "text") {
            textSlot = &s;
            break;
        }
    }
    if (textSlot) {
        TextValues values;
        std::string terr;
        bool ok = readTextValues(textSlot->gameSii, &values, &terr);
        std::string detail =
            ok ? fmt("(现金=%s, 经验=%s)",
                     values.hasMoney ? formatInt(values.money).c_str() : "无",
                     values.hasExperience ? formatInt(values.experience).c_str() : "无")
               : terr;
        check("读取明文存档数值", ok && (values.hasMoney || values.hasExperience), detail);

        wchar_t tempDir[MAX_PATH] = {0};
        ::GetTempPathW(MAX_PATH, tempDir);
        std::wstring tempFile = std::wstring(tempDir) + L"ets2_trainer_selftest.sii";
        std::vector<uint8_t> raw;
        if (readFileBytes(textSlot->gameSii, raw) && !raw.empty()) {
            writeFileBytes(tempFile, raw);
            std::string perr;
            bool patched = patchTextSave(tempFile, true, 123456789, true, 654321, &perr);
            TextValues after;
            readTextValues(tempFile, &after, nullptr);
            check("修改明文存档(副本)",
                  patched && after.hasMoney && after.money == 123456789 && after.hasExperience &&
                      after.experience == 654321,
                  patched ? fmt("(改后 现金=%s, 经验=%s)", formatInt(after.money).c_str(),
                                formatInt(after.experience).c_str())
                          : perr);
            std::vector<uint8_t> again;
            readFileBytes(tempFile, again);
            check("改动后文件大小变化很小",
                  again.size() + 8 > raw.size() && again.size() < raw.size() + 8,
                  fmt("(%llu -> %llu 字节)", (unsigned long long)raw.size(),
                      (unsigned long long)again.size()));
            ::DeleteFileW(tempFile.c_str());
        }
    } else {
        note("(没有明文存档可测试，跳过)");
    }

    // ---------- 5. 备份 ----------
    note("");
    note("=== 5. 备份 / 还原 ===");
    if (!slots.empty()) {
        std::string berr;
        std::wstring dir = backupSlot(slots.front(), "selftest", &berr);
        check("备份存档", !dir.empty(), dir.empty() ? berr : W2U(dir));
        std::vector<BackupInfo> backups = listBackups();
        check("列出备份", !backups.empty(), fmt("(%d 个)", (int)backups.size()));
        if (!dir.empty()) {  // 清理自检产生的备份
            std::wstring cmd = L"cmd.exe /c rmdir /s /q \"" + dir + L"\"";
            std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
            mutableCmd.push_back(0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                ::WaitForSingleObject(pi.hProcess, 5000);
                ::CloseHandle(pi.hProcess);
                ::CloseHandle(pi.hThread);
            }
        }
    }

    // ---------- 6. 控制台按键 ----------
    note("");
    note("=== 6. 控制台按键识别 ===");
    if (!slots.empty()) {
        int vk = consoleKeyFromControls(slots.front().profileDir + L"\\controls.sii");
        check("从 controls.sii 识别控制台按键", vk != 0, fmt("(0x%02X)", vk));
    }

    // ---------- 7. 界面字段绑定 ----------
    note("");
    note("=== 7. 界面字段绑定 ===");
    std::string bindingError;
    check("现金/经验/油量/损坏面板绑定", validatePanelBindings(&bindingError), bindingError);

    // ---------- 8. 遥测共享内存布局 ----------
    note("");
    note("=== 8. SCS 遥测布局 ===");
    std::vector<uint8_t> telemetryBytes(1056, 0);
    auto storeTelemetry = [&telemetryBytes](size_t offset, const auto& value) {
        ::memcpy(telemetryBytes.data() + offset, &value, sizeof(value));
    };
    const uint8_t active = 1;
    const uint32_t revision = 12, game = 1;
    const float scale = 3.0f, capacity = 1000.0f, fuel = 625.5f;
    const float wears[] = {0.01f, 0.02f, 0.03f, 0.04f, 0.05f};
    storeTelemetry(0, active);
    storeTelemetry(40, revision);
    storeTelemetry(52, game);
    storeTelemetry(700, scale);
    storeTelemetry(704, capacity);
    storeTelemetry(1000, fuel);
    for (size_t i = 0; i < 5; ++i) storeTelemetry(1036 + i * 4, wears[i]);
    TelemetrySnapshot telemetry;
    std::string telemetryError;
    bool telemetryOk = decodeTelemetryBuffer(telemetryBytes.data(), telemetryBytes.size(),
                                               &telemetry, &telemetryError);
    check("解析 revision 12 遥测油量/五项损伤",
          telemetryOk && std::fabs(telemetry.fuel - fuel) < 0.001f &&
              std::fabs(telemetry.wear[4] - wears[4]) < 0.0001f,
          telemetryError);

    // ---------- 9. 发动机动力调节状态机（离线，不依赖游戏）----------
    note("");
    note("=== 9. 发动机动力调节（内存镜像状态机测试）===");
    for (const auto& item : runEngineTunerTests()) {
        check(item.name, item.ok, item.detail);
    }

    // ---------- 10. 飞行模式辅助函数（纯字符串，不需要游戏）----------
    note("");
    note("=== 10. 飞行模式（config.cfg 改写 / controls.sii 相机按键）===");
    {
        const std::string sample =
            "uset g_developer \"0\"\r\nuset g_console \"1\"\r\nuset g_flyspeed \"100.0\"\r\n"
            "uset r_fullscreen \"1\"\r\n";
        std::string value;
        bool readOk = readConfigKey(sample, "g_console", &value) && value == "1";
        readOk = readOk && readConfigKey(sample, "g_flyspeed", &value) && value == "100.0";
        readOk = readOk && !readConfigKey(sample, "g_missing_key", &value);
        check("读取 config.cfg 键值", readOk);

        std::string text = sample;
        bool changed = false;
        rewriteConfigKey(&text, "g_developer", "1", &changed);
        const bool developer = changed && readConfigKey(text, "g_developer", &value) && value == "1";
        bool others = text.find("uset g_console \"1\"") != std::string::npos &&
                      text.find("uset g_flyspeed \"100.0\"") != std::string::npos &&
                      text.find("uset r_fullscreen \"1\"") != std::string::npos &&
                      text.find("\r\n") != std::string::npos;
        check("改写单个键且其它行原样保留（含 CRLF）", developer && others);

        std::string appended = sample;
        bool changed2 = false;
        rewriteConfigKey(&appended, "g_flyspeed", "300.0", &changed2);
        const bool speedOk =
            changed2 && readConfigKey(appended, "g_flyspeed", &value) && value == "300.0";
        bool noop = false;
        rewriteConfigKey(&appended, "g_flyspeed", "300.0", &noop);
        check("改写已有键值 / 同值时不重复改动", speedOk && !noop);

        std::string without = "uset g_developer \"1\"\n";
        bool changed3 = false;
        rewriteConfigKey(&without, "g_flyspeed", "500", &changed3);
        check("缺少键时追加到末尾",
              changed3 && readConfigKey(without, "g_flyspeed", &value) && value == "500");

        const std::string controls =
            "config_lines[223]: \"mix cam1 `keyboard.key1?0 | semantical.cam1?0`\"\n"
            "config_lines[232]: \"mix camdbg `keyboard.key0?0`\"\n"
            "config_lines[239]: \"mix camfwd `keyboard.num8?0 | semantical.camfwd?0`\"\n"
            "config_lines[240]: \"mix camback `keyboard.num5?0 | semantical.camback?0`\"\n"
            "config_lines[243]: \"mix camup `keyboard.num9?0 | semantical.camup?0`\"\n"
            "config_lines[244]: \"mix camdown `keyboard.num3?0 | semantical.camdown?0`\"\n";
        CameraBindings bindings;
        const bool parsed = parseCameraBindings(controls, &bindings);
        check("识别 controls.sii 的自由相机按键",
              parsed && bindings.toggle == '0' && bindings.forward == VK_NUMPAD8 &&
                  bindings.back == VK_NUMPAD5 && bindings.up == VK_NUMPAD9 &&
                  bindings.down == VK_NUMPAD3,
              fmt("(开关 0x%02X 前进 0x%02X 上升 0x%02X)", bindings.toggle, bindings.forward,
                  bindings.up));
        check("按键名转虚拟键码",
              keyNameToVirtualKey("key7") == '7' &&
                  keyNameToVirtualKey("num0") == VK_NUMPAD0 &&
                  keyNameToVirtualKey("grave") == 0xC0 && keyNameToVirtualKey("unknown") == 0);
    }

    // ---------- 11. 地图传送的城市名提取（纯字符串，不需要游戏）----------
    note("");
    note("=== 11. 地图传送（存档城市名提取）===");
    {
        const std::string sample =
            "tradeaux.berlin posped.berlin lkwlog.hamburg transinet.hamburg "
            "tradeaux.berlinb posped.berlinb volvo.fh_2021 volvo.fh16 scania.s_2016 x.y";
        const std::vector<uint8_t> bytes(sample.begin(), sample.end());
        const std::vector<std::string> cities = extractMapCities(bytes);
        const bool onlyReal = cities.size() == 2 && cities[0] == "berlin" && cities[1] == "hamburg";
        check("提取城市名：≥2 家公司配对、排除车型名与仓库后缀", onlyReal,
              fmt("(得到 %d 个)", (int)cities.size()));
    }

    // ---------- 12. 联运（Convoy）会话解析（纯字符串，不需要游戏）----------
    note("");
    note("=== 12. 联运模式（game.log.txt 会话解析）===");
    {
        const std::string log =
            "00:52:43.896 : [MP] Session started.\n"
            "00:52:44.540 : [MP] Game server joined.\n"
            "00:52:59.717 : [MP] 76561198000000001 connected, client_id = 83\n"
            "00:52:59.717 : [MP] PLAYER_B connected, client_id = 109\n"
            "00:52:59.871 : [MP] Session running.\n"
            "00:52:59.929 : [MP] PLAYER_A connected, client_id = 115 [you]\n";
        ConvoySession session;
        const bool parsed = parseConvoySession(log, &session);
        check("解析 Convoy 会话与在线玩家",
              parsed && session.active && session.players.size() == 3 &&
                  session.startedAt == "00:52:43.896",
              fmt("(%d 人，开始 %s)", (int)session.players.size(), session.startedAt.c_str()));
        bool youMarked = false;
        for (const auto& player : session.players) {
            if (player.you && player.name == "PLAYER_A" && player.clientId == 115) youMarked = true;
        }
        check("标记本地玩家 [you]", youMarked);

        const std::string closed =
            log + "00:54:09.000 : [MP] 76561198000000001 disconnected, client_id = 83\n"
                  "00:54:09.001 : [MP] Session closed.\n";
        ConvoySession afterClose;
        parseConvoySession(closed, &afterClose);
        check("会话结束后不再上报玩家与联机状态",
              !afterClose.active && afterClose.players.empty());
    }

    // ---------- 13. 金钱 / 经验结构化定位校验（在自身进程里构造假对象）----------
    note("");
    note("=== 13. 金钱 / 经验结构化定位（单位字段表校验）===");
    {
        // 模拟 bank 对象：money_account s64@0x18、免赔额/共保比例/贷款额度等浮点字段
        std::vector<uint8_t> bankBuf(0x100, 0);
        const int64_t fakeMoney = 123456789;
        const float coinsuranceFixed = 500.0f;
        const float coinsuranceRatio = 0.2f;
        const float loanLimit = 100000.0f;
        const float overdraft = 0.0f;
        const int64_t appEnabled = 1;
        const int32_t overdraftTimer = 30;
        ::memcpy(bankBuf.data() + economy_offsets::kBankMoneyAccount, &fakeMoney, sizeof(fakeMoney));
        ::memcpy(bankBuf.data() + economy_offsets::kBankCoinsuranceFixed, &coinsuranceFixed,
                 sizeof(coinsuranceFixed));
        ::memcpy(bankBuf.data() + economy_offsets::kBankCoinsuranceRatio, &coinsuranceRatio,
                 sizeof(coinsuranceRatio));
        ::memcpy(bankBuf.data() + economy_offsets::kBankLoanLimit, &loanLimit, sizeof(loanLimit));
        ::memcpy(bankBuf.data() + economy_offsets::kBankOverdraft, &overdraft, sizeof(overdraft));
        ::memcpy(bankBuf.data() + economy_offsets::kBankOverdraftTimer, &overdraftTimer,
                 sizeof(overdraftTimer));
        ::memcpy(bankBuf.data() + economy_offsets::kBankAppEnabled, &appEnabled,
                 sizeof(appEnabled));
        const uint64_t bankBase = (uint64_t)(uintptr_t)bankBuf.data();

        // 模拟经济对象：bank 引用、游戏时间、存档版本、经验与 6 项技能点
        std::vector<uint8_t> econBuf(0x800, 0);
        const int32_t saveVersion = 50;
        const int32_t fakeXp = 582499;
        const int32_t skills[6] = {3, 5, 2, 1, 4, 2};
        const int32_t gameTimeSecs = 123456;
        ::memcpy(econBuf.data() + economy_offsets::kEconomyBankRef, &bankBase, sizeof(bankBase));
        ::memcpy(econBuf.data() + economy_offsets::kEconomyGameTimeSecs, &gameTimeSecs,
                 sizeof(gameTimeSecs));
        ::memcpy(econBuf.data() + economy_offsets::kEconomySaveGameVersion, &saveVersion,
                 sizeof(saveVersion));
        ::memcpy(econBuf.data() + economy_offsets::kEconomyExperiencePoints, &fakeXp,
                 sizeof(fakeXp));
        for (int i = 0; i < economy_offsets::kEconomySkillCount; ++i) {
            ::memcpy(econBuf.data() + economy_offsets::kEconomySkills + (size_t)i * 4, &skills[i],
                     sizeof(int32_t));
        }
        const uint64_t econBase = (uint64_t)(uintptr_t)econBuf.data();

        ProcessMemory selfMem;
        std::string selfErr;
        const bool selfOpen = selfMem.open(::GetCurrentProcessId(), "self-economy", &selfErr);

        ProbeResult bankProbe;
        const bool bankOk = selfOpen &&
                            probeBankObject(selfMem, bankBase + economy_offsets::kBankMoneyAccount,
                                            &bankProbe);
        check("bank 对象校验（金额 + 浮点字段）", bankOk && bankProbe.object == bankBase &&
                                                      bankProbe.score >= 4,
              fmt("(得分 %d/%d，金额 %s)", bankProbe.score, bankProbe.maxScore,
                  formatInt(bankProbe.value).c_str()));

        ProbeResult econProbe;
        const bool econOk =
            selfOpen && probeEconomyObject(selfMem,
                                           econBase + economy_offsets::kEconomyExperiencePoints,
                                           &econProbe);
        check("经济对象校验（经验 + 技能点）", econOk && econProbe.object == econBase &&
                                                       econProbe.score >= 5,
              fmt("(得分 %d/%d)", econProbe.score, econProbe.maxScore));

        check("经济对象 → bank 引用一致",
              selfOpen && economyReferencesBank(selfMem, econBase, bankBase));

        // 反向：把「经验地址」故意指错（偏移 -4，落在存档版本字段上）应判定失败
        ProbeResult wrongProbe;
        const bool wrongOk = selfOpen && probeEconomyObject(
                                            selfMem, econBase + economy_offsets::kEconomySaveGameVersion,
                                            &wrongProbe);
        check("偏移错误时拒绝（不会误判地址）", !wrongOk,
              fmt("(误判得分 %d/%d)", wrongProbe.score, wrongProbe.maxScore));

        // 指针槽 → 经济对象地址换算
        check("由 bank 指针反推经济对象",
              economyFromBankPointer(econBase + economy_offsets::kEconomyBankRef) == econBase);

        // bank 结构被破坏（共保比例变成 9999）时得分应下降
        float brokenRatio = 9999.0f;
        ::memcpy(bankBuf.data() + economy_offsets::kBankCoinsuranceRatio, &brokenRatio,
                 sizeof(brokenRatio));
        ProbeResult brokenProbe;
        const bool brokenOk =
            selfOpen && probeBankObject(selfMem, bankBase + economy_offsets::kBankMoneyAccount,
                                        &brokenProbe);
        check("字段异常时得分下降", !brokenOk || brokenProbe.score < bankProbe.score,
              fmt("(异常得分 %d，正常得分 %d)", brokenProbe.score, bankProbe.score));

        // ---------- 直接写入（免锁定）路径：结构校验 + 范围检查 + 回读比对 ----------
        // 先把 bank 恢复成合法结构，再走一次真实的直接写入
        ::memcpy(bankBuf.data() + economy_offsets::kBankCoinsuranceRatio, &coinsuranceRatio,
                 sizeof(coinsuranceRatio));
        const int64_t newMoney = 999999999;
        std::string writeError;
        const bool moneyWriteOk =
            selfOpen && writeBankMoneyVerified(selfMem,
                                               bankBase + economy_offsets::kBankMoneyAccount,
                                               bankBase, newMoney, &writeError);
        int64_t moneyAfter = 0;
        ::memcpy(&moneyAfter, bankBuf.data() + economy_offsets::kBankMoneyAccount,
                 sizeof(moneyAfter));
        check("直接写入金钱（校验 + 身份 + 回读一致）",
              moneyWriteOk && moneyAfter == newMoney,
              moneyWriteOk ? "(写入后内存值一致)" : ("(" + writeError + ")"));

        const bool moneyRangeOk =
            selfOpen && writeBankMoneyVerified(selfMem,
                                               bankBase + economy_offsets::kBankMoneyAccount,
                                               bankBase, 9999999999999999LL, &writeError);
        check("金额越界时拒绝写入", !moneyRangeOk, ("(" + writeError + ")"));

        const bool moneyWrongOk =
            selfOpen && writeBankMoneyVerified(selfMem, econBase + 0x18, bankBase, newMoney,
                                               &writeError);
        check("非 bank 地址拒绝写入", !moneyWrongOk,
              ("(" + (moneyWrongOk ? std::string("误通过") : writeError) + ")"));

        const int32_t newXp = 1500000;
        const bool xpWriteOk =
            selfOpen && writeEconomyExperienceVerified(
                            selfMem, econBase + economy_offsets::kEconomyExperiencePoints,
                            econBase, newXp, &writeError);
        int32_t xpAfter = 0;
        ::memcpy(&xpAfter, econBuf.data() + economy_offsets::kEconomyExperiencePoints,
                 sizeof(xpAfter));
        check("直接写入经验（校验 + 身份 + 回读一致）",
              xpWriteOk && xpAfter == newXp,
              xpWriteOk ? "(写入后内存值一致)" : ("(" + writeError + ")"));

        const bool xpRangeOk =
            selfOpen && writeEconomyExperienceVerified(
                            selfMem, econBase + economy_offsets::kEconomyExperiencePoints,
                            econBase, 2100000000, &writeError);
        check("经验越界时拒绝写入", !xpRangeOk, ("(" + writeError + ")"));
    }

    // ---------- 13b. 经济定位候选筛选（零/唯一/同分歧义 + 地址换算）----------
    note("");
    note("=== 13b. 经济定位候选筛选（歧义必须失败关闭）===");
    for (const auto& item : runEconomyLocateTests()) {
        check(item.name, item.ok, item.detail);
    }

    // ---------- 汇总 ----------
    std::string summary = fmt("自检结果：通过 %d 项，失败 %d 项", g_pass, g_fail);
    g_report.push_back("");
    g_report.push_back("================ 汇总 ================");
    g_report.push_back(summary);

    std::string text;
    for (const auto& line : g_report) {
        text += line;
        text += "\r\n";
    }
    std::vector<uint8_t> bytes(text.begin(), text.end());
    writeFileBytes(outPath, bytes);

    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            std::string msg = summary + "\r\nreport -> " + W2U(outPath) + "\r\n";
            DWORD written = 0;
            ::WriteConsoleA(h, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
        }
    }
    return g_fail == 0 ? 0 : 1;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    ::ImGui_ImplWin32_EnableDpiAwareness();

    // 命令行：支持 --selftest 与 --selftest=<报告文件路径>
    std::wstring cmdLine = lpCmdLine ? lpCmdLine : L"";
    const std::wstring lifecycleFlag = L"--lifecycle-helper=";
    if (cmdLine.find(lifecycleFlag) == 0) {
        const std::wstring eventName = cmdLine.substr(lifecycleFlag.size());
        HANDLE event = ::OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
        if (!event) return 10;
        const DWORD waited = ::WaitForSingleObject(event, 10000);
        ::CloseHandle(event);
        return waited == WAIT_OBJECT_0 ? 259 : 11;
    }
    // 命令行 --game=ets2 / --game=ats：必须在任何进程/存档探测之前确定目标游戏。
    {
        const size_t gamePos = cmdLine.find(L"--game=");
        if (gamePos != std::wstring::npos) {
            std::wstring key = cmdLine.substr(gamePos + 7);
            const size_t stop = key.find_first_of(L" \t\"");
            if (stop != std::wstring::npos) key = key.substr(0, stop);
            GameId parsed;
            if (parseGameKey(W2U(key), &parsed)) { setSelectedGame(parsed); lockSelectionFromCli(); }
        }
    }
    if (cmdLine.find(L"--target-helper=") != std::wstring::npos) {
        size_t pos = cmdLine.find(L"--target-helper=") + 16;
        std::wstring path = cmdLine.substr(pos);
        while (!path.empty() && (path.front() == L'"' || path.front() == L' ')) {
            path.erase(path.begin());
        }
        while (!path.empty() && (path.back() == L'"' || path.back() == L' ')) path.pop_back();
        return runTargetHelper(path);
    }
    if (cmdLine.find(L"--crosstest") != std::wstring::npos) {
        return runCrossTest(applicationDir() + L"\\cross_process_report.txt");
    }
    if (cmdLine.find(L"--livetest") != std::wstring::npos) {
        return runLiveTest(applicationDir() + L"\\live_test_report.txt");
    }
    if (cmdLine.find(L"--enginetest") != std::wstring::npos) {
        return runEngineProbeTest(applicationDir() + L"\\engine_probe_report.txt");
    }
    if (cmdLine.find(L"--fieldprobe") != std::wstring::npos) {
        return runFieldProbe(applicationDir() + L"\\field_probe_report.txt");
    }
    if (cmdLine.find(L"--flycheck") != std::wstring::npos) {
        return runFlyCheck(applicationDir() + L"\\fly_check_report.txt");
    }
    if (cmdLine.find(L"--citycheck") != std::wstring::npos) {
        return runCityCheck(applicationDir() + L"\\city_check_report.txt");
    }
    if (cmdLine.find(L"--convoycheck") != std::wstring::npos) {
        return runConvoyCheck(applicationDir() + L"\\convoy_check_report.txt");
    }
    if (cmdLine.find(L"--powertest") != std::wstring::npos) {
        return runPowerTest(applicationDir() + L"\\power_test_report.txt");
    }
    if (cmdLine.find(L"--parsetest") != std::wstring::npos) {
        return runParseTest();
    }
    if (cmdLine.find(L"--selftest") != std::wstring::npos) {
        std::wstring path;
        size_t eq = cmdLine.find(L"--selftest=");
        if (eq != std::wstring::npos) {
            path = cmdLine.substr(eq + 11);
            while (!path.empty() && (path.front() == L'"' || path.front() == L' ')) {
                path.erase(path.begin());
            }
            while (!path.empty() && (path.back() == L'"' || path.back() == L' ')) {
                path.pop_back();
            }
        }
        if (path.empty()) {
            wchar_t buf[MAX_PATH * 2];
            DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
            std::wstring exe(buf, n);
            size_t slash = exe.find_last_of(L"\\/");
            path = ((slash == std::wstring::npos) ? exe : exe.substr(0, slash)) +
                   L"\\selftest_report.txt";
        }
        return runSelfTest(path);
    }

    if (cmdLine.find(L"--gametest") != std::wstring::npos) {
        return runGameSelftest(applicationDir() + L"\\gametest_report.txt");
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInstance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ETS2TrainerWnd";
    ::RegisterClassExW(&wc);

    RECT workArea{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int workWidth = workArea.right - workArea.left;
    const int workHeight = workArea.bottom - workArea.top;
    const int initialWidth = (std::min)(1280, (std::max)(1000, workWidth - 40));
    const int initialHeight = (std::min)(820, (std::max)(650, workHeight - 40));
    const int initialX = workArea.left + (std::max)(0, (workWidth - initialWidth) / 2);
    const int initialY = workArea.top + (std::max)(0, (workHeight - initialHeight) / 2);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"欧卡2 / 美卡 修改器  Truck Trainer",
                                WS_OVERLAPPEDWINDOW, initialX, initialY, initialWidth, initialHeight,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    applyDarkTitleBar(hwnd);

    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ::MessageBoxW(nullptr, L"创建 Direct3D 11 设备失败，无法启动界面。", L"错误",
                      MB_ICONERROR);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ::ImGui::CreateContext();
    ImGuiIO& io = ::ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // 不生成 imgui.ini
    applyModernStyle();
    ::ImGui_ImplWin32_Init(hwnd);
    ::ImGui_ImplDX11_Init(g_device, g_context);

    AppState app;
    g_app = &app;
    loadSettings(app);
    SYSTEMTIME logTime;
    ::GetLocalTime(&logTime);
    const std::filesystem::path logDir = std::filesystem::path(applicationDir()) / L"logs";
    std::error_code logEc;
    std::filesystem::create_directories(logDir, logEc);
    const std::wstring logName = fmtW(L"trainer_%04d%02d%02d_%02d%02d%02d_%03d_%u.log",
        logTime.wYear, logTime.wMonth, logTime.wDay, logTime.wHour, logTime.wMinute,
        logTime.wSecond, logTime.wMilliseconds, ::GetCurrentProcessId());
    std::string logError;
    if (!logStartFile((logDir / logName).wstring(), &logError)) logLine(logError);
    logLine(fmt("会话开始：PID=%u game=%s exe=%s 管理员=%s", ::GetCurrentProcessId(),
        gameKey(selectedGame()), W2U(selectedProcessName()).c_str(), isAdmin() ? "是" : "否"));
    initFonts(app);
    refreshSaves(app);
    refreshFlyMode(app);      // 读取 config.cfg 的开发者/控制台/飞行速度状态
    detectCameraKeys(app);    // 从 controls.sii 识别自由相机按键
    refreshConvoy(app);       // 读取联机状态与 Convoy 会话/玩家列表
    {
        DWORD pid = 0;
        std::wstring exe;
        if (findProcess(selectedProcessName(), &pid, &exe)) {
            app.procStatus = fmt("发现 %s (PID %u) —— 点「附加游戏」开始", W2U(exe).c_str(), pid);
        } else {
            app.procStatus = fmt("没有找到 %s，请先启动游戏", W2U(selectedProcessName()).c_str());
        }
        logLine(fmt("欧卡2 / 美卡 修改器已启动，当前目标：%s。", selectedGameDescriptor().shortName));
        logLine(isAdmin() ? "当前是管理员权限。" : "当前不是管理员，若读取失败请以管理员身份运行。");
        logLine(app.procStatus);
    }

    bool done = false;
    bool smokeTest = cmdLine.find(L"--smoketest") != std::wstring::npos;
    const bool saveCopyUiSmoke = smokeTest && cmdLine.find(L"--savecopy-ui") != std::wstring::npos;
    const bool truckTransferUiSmoke = smokeTest && cmdLine.find(L"--trucktransfer-ui") != std::wstring::npos;
    const bool truckTransferClickUiSmoke = smokeTest && cmdLine.find(L"--trucktransfer-click-ui") != std::wstring::npos;
    const bool profileSplitUiSmoke = smokeTest && cmdLine.find(L"--profilesplit-ui") != std::wstring::npos;
    if (profileSplitUiSmoke) {
        app.activeTab = 4; app.splitGame = selectedGame(); app.splitUiFixture = true;
        for (const char* name : {"TMP", "ProMods", "联运"}) {
            ProfileSplitRow row; row.selected = true;
            row.source.profileName = L"界面测试档案"; row.source.displayName = name;
            row.source.slotName = std::to_wstring(app.splitRows.size()+3);
            ::strcpy_s(row.name, name); app.splitRows.push_back(row);
        }
        app.showProfileSplit = true;
    }
    if(truckTransferClickUiSmoke) {
        app.activeTab=4;
        app.slots.clear();app.selectedSlot=-1;
        app.truckTransferUiProbe.capture=true;
    }
    if (truckTransferUiSmoke) {
        app.activeTab = 4;
        app.truckTransferRequest.game = selectedGame();
        app.truckTransferRequest.source.slotDir = L"ui-source-not-a-save";
        app.truckTransferRequest.source.label = "源档案 / 测试存档";
        SaveSlot target; target.slotDir = L"ui-target-not-a-save"; target.label = "目标档案 / 测试存档";
        app.transferTargets = {target};
        app.truckTransferRequest.target = target;
        app.transferTargetIndex = 0;
        app.transferSourceInventory.trucks = {{"_nameless.1", "volvo.fh_2024", 52}};
        app.transferTargetInventory.garages = {{"garage.koln", 2}};
        ::strcpy_s(app.transferName, "车辆导入副本");
        app.transferNote = "界面冒烟：模拟源卡车与目标空车库，不执行导入。";
        app.showTruckTransfer = true;
    }
    if (saveCopyUiSmoke) {
        app.activeTab = 4;
        app.copyRequest.game = selectedGame();
        app.copyRequest.source.profileName = L"界面测试档案";
        app.copyRequest.source.slotName = L"1";
        app.copyRequest.source.slotDir = L"ui-fixture-not-a-save";
        ::strcpy_s(app.copyName, "中文副本 \"测试\"");
        app.showCopyDialog = true;
    }
    uint64_t startTick = ::GetTickCount64();
    int frames = 0;
    int smokeResult = 0;
    while (!done) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ::ImGui_ImplDX11_NewFrame();
        ::ImGui_ImplWin32_NewFrame();
        if(truckTransferClickUiSmoke && (frames==2 || frames==3)) {
            auto& input=::ImGui::GetIO();
            input.AddFocusEvent(true);
            input.AddMousePosEvent(app.truckTransferUiProbe.buttonX,app.truckTransferUiProbe.buttonY);
            input.AddMouseButtonEvent(0,frames==2);
        }
        ::ImGui::NewFrame();
        handleGlobalHotkeys(app);
        renderApp(app);
        ::ImGui::Render();

        const float clearColor[4] = {0.086f, 0.094f, 0.110f, 1.00f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clearColor);
        ::ImGui_ImplDX11_RenderDrawData(::ImGui::GetDrawData());
        g_swapChain->Present(1, 0);  // 垂直同步，避免白烧显卡
        ++frames;

        if (smokeTest && frames >= 30) {
            if(truckTransferClickUiSmoke) {
                const auto& probe=app.truckTransferUiProbe;
                const bool ok=probe.clicked && probe.noticeVisible && !app.truckTransferNotice.empty() && !app.worker.joinable();
                smokeResult=ok ? 0 : 1;
                logLine(fmt("[ %s ] 车辆复制按钮点击：clicked=%d notice_visible=%d worker_started=%d",ok ? "PASS" : "FAIL",
                            probe.clicked,probe.noticeVisible,app.worker.joinable()));
                logLine("车辆复制点击提示："+app.truckTransferNotice);
            }
            if (profileSplitUiSmoke) logLine("档案拆分界面冒烟：三个存档的选择、独立档案命名与提示已渲染；未创建档案。");
            if (truckTransferUiSmoke) logLine("车辆导入界面冒烟：源卡车、目标存档、空车库与名称弹窗已渲染；未执行导入。");
            if (saveCopyUiSmoke) logLine("存档复制界面冒烟：列表与复制名称弹窗已渲染；未执行复制。");
            logLine(fmt("冒烟测试：窗口创建成功，已渲染 %d 帧（hwnd=0x%llX, 窗口 %ux%u）", frames,
                        (unsigned long long)(uintptr_t)hwnd, g_width, g_height));
            logLine(fmt("界面构造：标签页 6 个（含「车辆 油量/无损/动力」），存档槽 %d 个，中文字体 %s",
                        (int)app.slots.size(),
                        ::ImGui::GetIO().Fonts->Fonts.Size > 0 ? "已加载" : "未加载"));
            logSaveToFile(applicationDir() + L"\\smoketest_report.txt");
            done = true;
        }
        if (smokeTest && ::GetTickCount64() - startTick > 15000) {
            smokeResult=1;
            logLine("冒烟测试：超时退出");
            logSaveToFile(applicationDir() + L"\\smoketest_report.txt");
            done = true;
        }
    }

    // ---------- 退出清理 ----------
    app.cancel.store(true);
    if (app.worker.joinable()) app.worker.join();
    if (app.vehicleLocker) app.vehicleLocker->stop();
    if (app.engineTuner) {
        // 尽力写回发动机原值，然后才放弃内存引用。
        app.engineTuner->restoreAndDetach();
    }
    if (app.freezer) app.freezer->stop();
    app.mem.close();
    saveSettings(app);
    logLine("会话正常退出：已停止后台线程并保存设置。");
    logSaveToFile(applicationDir() + L"\\ETS2Trainer.log");
    logStopFile();
    ::ImGui_ImplDX11_Shutdown();
    ::ImGui_ImplWin32_Shutdown();
    ::ImGui::DestroyContext();
    cleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return smokeResult;
}


