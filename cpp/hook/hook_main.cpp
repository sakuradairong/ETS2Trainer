#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT(WINAPI* PFN_ResizeBuffers)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                          UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

PFN_Present       g_origPresent       = nullptr;
PFN_ResizeBuffers g_origResizeBuffers = nullptr;
void**            g_swapChainVTable   = nullptr;

ID3D11Device*           g_d3dDevice        = nullptr;
ID3D11DeviceContext*    g_d3dContext       = nullptr;
ID3D11RenderTargetView* g_renderTargetView = nullptr;
HWND                    g_gameHwnd         = nullptr;
WNDPROC                 g_origWndProc      = nullptr;

bool g_imguiInitialized = false;
bool g_menuVisible      = false;

// 游戏状态开关
bool g_infiniteFuel   = false;
bool g_noDamage       = false;
int  g_enginePowerIdx = 0; // 0=原厂, 1=1.25x, 2=1.5x, 3=2.0x

// 寻找 eurotrucks2.exe 顶层窗口
HWND findGameWindow() {
    DWORD currentPid = ::GetCurrentProcessId();
    HWND hwnd = nullptr;
    while ((hwnd = ::FindWindowExW(nullptr, hwnd, nullptr, nullptr)) != nullptr) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid == currentPid && ::IsWindowVisible(hwnd)) {
            wchar_t title[256] = {0};
            ::GetWindowTextW(hwnd, title, 255);
            if (wcsstr(title, L"Euro Truck Simulator 2") || wcsstr(title, L"ETS2") || wcsstr(title, L"Prism3D")) {
                return hwnd;
            }
        }
    }
    return nullptr;
}

LRESULT WINAPI hookWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && (wParam == VK_INSERT || wParam == VK_HOME)) {
        g_menuVisible = !g_menuVisible;
        return 0;
    }

    if (g_menuVisible) {
        if (::ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
            return true;
        }
        ImGuiIO& io = ::ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
            // 当菜单呼出并捕获鼠标时，拦截游戏本身的鼠标/键盘消息，避免开车时打方向
            if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return true;
            if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wParam != VK_INSERT && wParam != VK_HOME) return true;
        }
    }

    return ::CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

void cleanupRenderTarget() {
    if (g_renderTargetView) {
        g_renderTargetView->Release();
        g_renderTargetView = nullptr;
    }
}

void createRenderTarget(IDXGISwapChain* pSwapChain) {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer))) {
        g_d3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_renderTargetView);
        pBackBuffer->Release();
    }
}

void applyHookTheme() {
    ::ImGui::StyleColorsDark();
    ImGuiStyle& style = ::ImGui::GetStyle();
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 6.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 6);

    style.Colors[ImGuiCol_WindowBg]         = ImVec4(0.08f, 0.09f, 0.12f, 0.92f);
    style.Colors[ImGuiCol_Border]           = ImVec4(0.20f, 0.40f, 0.75f, 0.80f);
    style.Colors[ImGuiCol_FrameBg]          = ImVec4(0.14f, 0.16f, 0.22f, 0.85f);
    style.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.20f, 0.25f, 0.35f, 0.90f);
    style.Colors[ImGuiCol_FrameBgActive]    = ImVec4(0.25f, 0.32f, 0.45f, 1.00f);
    style.Colors[ImGuiCol_TitleBg]          = ImVec4(0.12f, 0.14f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive]    = ImVec4(0.16f, 0.38f, 0.72f, 1.00f);
    style.Colors[ImGuiCol_Button]           = ImVec4(0.16f, 0.38f, 0.72f, 0.90f);
    style.Colors[ImGuiCol_ButtonHovered]    = ImVec4(0.22f, 0.48f, 0.88f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive]     = ImVec4(0.12f, 0.30f, 0.60f, 1.00f);
    style.Colors[ImGuiCol_CheckMark]        = ImVec4(0.22f, 0.85f, 0.52f, 1.00f);
}

// 渲染游戏内浮动菜单
void renderInGameMenu() {
    if (!g_menuVisible) return;

    ::ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    if (::ImGui::Begin("ETS2 游戏内内置修改菜单 [Insert / Home 呼出/隐藏]###ets2_overlay", &g_menuVisible,
                       ImGuiWindowFlags_NoCollapse)) {
        
        ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ 快捷车载控制");
        ::ImGui::Separator();
        
        if (::ImGui::Checkbox("无限油量 (自动满油)", &g_infiniteFuel)) {
            // TODO: 调用内部或共享内存开关
        }
        ::ImGui::SameLine(220);
        if (::ImGui::Checkbox("车辆无损 (五项清零)", &g_noDamage)) {
            // TODO: 调用内部或共享内存开关
        }

        ::ImGui::Spacing();
        ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ 发动机动力档位");
        ::ImGui::Separator();
        static const char* kPowers[] = {"原厂 1.0x", "1.25x 轻度", "1.50x 超车", "2.00x 狂飙"};
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ::ImGui::SameLine();
            const bool active = (g_enginePowerIdx == i);
            if (active) ::ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.60f, 0.95f, 1.0f));
            if (::ImGui::Button(kPowers[i])) {
                g_enginePowerIdx = i;
            }
            if (active) ::ImGui::PopStyleColor();
        }

        ::ImGui::Spacing();
        ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ 快捷传送与导航");
        ::ImGui::Separator();
        if (::ImGui::Button("送达导航终点 (goto nav_end)", ImVec2(190, 26))) {
            // 内部直接模拟控制台或发送
            ::keybd_event(192, 0, 0, 0); // `
            ::keybd_event(192, 0, KEYEVENTF_KEYUP, 0);
        }
        ::ImGui::SameLine();
        if (::ImGui::Button("回到导航起点", ImVec2(190, 26))) {
            // goto nav_start
        }
        if (::ImGui::Button("撤销传送 (goto back)", ImVec2(190, 26))) {
            // goto back
        }
        ::ImGui::SameLine();
        if (::ImGui::Button("一键放下卡车 (Ctrl+F9)", ImVec2(190, 26))) {
            ::keybd_event(VK_CONTROL, 0, 0, 0);
            ::keybd_event(VK_F9, 0, 0, 0);
            ::keybd_event(VK_F9, 0, KEYEVENTF_KEYUP, 0);
            ::keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        }

        ::ImGui::Spacing();
        ::ImGui::TextColored(ImVec4(0.38f, 0.78f, 1.00f, 1.0f), "◆ 环境与时间控制");
        ::ImGui::Separator();
        if (::ImGui::Button("时间设为早晨 08:00")) {}
        ::ImGui::SameLine();
        if (::ImGui::Button("时间设为傍晚 20:00")) {}
        if (::ImGui::Button("清除违章/疲劳")) {}
        ::ImGui::SameLine();
        if (::ImGui::Button("清空道路交通")) {}

        ::ImGui::Spacing();
        ::ImGui::Separator();
        ::ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "按 [Insert] 或 [Home] 随时呼出或隐藏本菜单");
    }
    ::ImGui::End();
}

HRESULT WINAPI Hooked_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!g_imguiInitialized) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_d3dDevice))) {
            g_d3dDevice->GetImmediateContext(&g_d3dContext);
            DXGI_SWAP_CHAIN_DESC desc;
            pSwapChain->GetDesc(&desc);
            g_gameHwnd = desc.OutputWindow;
            if (!g_gameHwnd) g_gameHwnd = findGameWindow();

            createRenderTarget(pSwapChain);

            IMGUI_CHECKVERSION();
            ::ImGui::CreateContext();
            ImGuiIO& io = ::ImGui::GetIO();
            io.IniFilename = nullptr;
            applyHookTheme();

            ::ImGui_ImplWin32_Init(g_gameHwnd);
            ::ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);

            if (g_gameHwnd) {
                g_origWndProc = (WNDPROC)::SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)hookWndProc);
            }
            g_imguiInitialized = true;
        }
    }

    if (g_imguiInitialized && g_renderTargetView) {
        ::ImGui_ImplDX11_NewFrame();
        ::ImGui_ImplWin32_NewFrame();
        ::ImGui::NewFrame();

        renderInGameMenu();

        ::ImGui::Render();
        g_d3dContext->OMSetRenderTargets(1, &g_renderTargetView, nullptr);
        ::ImGui_ImplDX11_RenderDrawData(::ImGui::GetDrawData());
    }

    return g_origPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT WINAPI Hooked_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                   UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    cleanupRenderTarget();
    HRESULT hr = g_origResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    createRenderTarget(pSwapChain);
    return hr;
}

DWORD WINAPI HookThread(LPVOID) {
    // 稍等游戏主模块加载并创建 D3D11 设备与 SwapChain
    ::Sleep(2000);

    // 创建虚拟窗口与虚拟 SwapChain 以获取 IDXGISwapChain 的 VTable
    WNDCLASSEX wc = {sizeof(WNDCLASSEX), CS_CLASSDC, ::DefWindowProc, 0L, 0L,
                     ::GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                     L"ETS2DummyClass", nullptr};
    ::RegisterClassEx(&wc);
    HWND dummyHwnd = ::CreateWindowW(wc.lpszClassName, L"Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                     nullptr, nullptr, wc.hInstance, nullptr);

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11Device* dummyDevice = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;

    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevels, 2, D3D11_SDK_VERSION,
        &scd, &dummySwapChain, &dummyDevice, &featureLevel, &dummyContext);

    if (SUCCEEDED(hr) && dummySwapChain) {
        void** vtable = *(void***)dummySwapChain;
        g_swapChainVTable = vtable;

        // VTable Index 8: Present
        // VTable Index 13: ResizeBuffers
        DWORD oldProtect = 0;
        ::VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
        g_origPresent = (PFN_Present)vtable[8];
        vtable[8] = (void*)&Hooked_Present;
        ::VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);

        ::VirtualProtect(&vtable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
        g_origResizeBuffers = (PFN_ResizeBuffers)vtable[13];
        vtable[13] = (void*)&Hooked_ResizeBuffers;
        ::VirtualProtect(&vtable[13], sizeof(void*), oldProtect, &oldProtect);

        dummySwapChain->Release();
        dummyDevice->Release();
        dummyContext->Release();
    }

    ::DestroyWindow(dummyHwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace

// SCS 原生插件导出接口：支持直接放入 plugins 目录自启加载
extern "C" __declspec(dllexport) bool scs_telemetry_init(const unsigned int version, const void* params) {
    ::CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    return true;
}

extern "C" __declspec(dllexport) void scs_telemetry_shutdown() {
    // 卸载处理
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(hinstDLL);
        ::CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_swapChainVTable && g_origPresent) {
            DWORD oldProtect = 0;
            ::VirtualProtect(&g_swapChainVTable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
            g_swapChainVTable[8] = (void*)g_origPresent;
            ::VirtualProtect(&g_swapChainVTable[8], sizeof(void*), oldProtect, &oldProtect);
        }
        if (g_swapChainVTable && g_origResizeBuffers) {
            DWORD oldProtect = 0;
            ::VirtualProtect(&g_swapChainVTable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
            g_swapChainVTable[13] = (void*)g_origResizeBuffers;
            ::VirtualProtect(&g_swapChainVTable[13], sizeof(void*), oldProtect, &oldProtect);
        }
        if (g_gameHwnd && g_origWndProc) {
            ::SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        }
    }
    return TRUE;
}
