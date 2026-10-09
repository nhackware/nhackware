// nhmenu_host.dll - injectable ImGui overlay for the emulator's Windows process
// (HD-Player.exe). It does NOT touch the Android guest: it just proves the
// injection path works and gives us an ImGui surface living inside the player.
//
// Inject with LoadLibrary mode (Extreme Injector: "LoadLibrary", not manual map).
// END key unloads the menu thread; the DLL then frees itself.
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

// imgui ships this declaration inside '#if 0'; copy it out as instructed.
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dCtx = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static ID3D11RenderTargetView *g_pRTV = nullptr;
static volatile bool g_running = true;
static HWND g_hwnd = nullptr;

static void CleanupDevice() {
    if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dCtx) { g_pd3dCtx->Release(); g_pd3dCtx = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
            if (g_pSwapChain) g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D *back = nullptr;
            if (g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back)) == S_OK && back) {
                g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_pRTV);
                back->Release();
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &got, &g_pd3dCtx);
    if (FAILED(hr)) return false;
    ID3D11Texture2D *back = nullptr;
    if (FAILED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_pRTV);
    back->Release();
    return g_pRTV != nullptr;
}

// the familiar red-bar menu, reused from the in-game one
static bool b_aim, b_esp, b_name, b_hp, b_speed;
static float g_alpha = 1.0f;

static void DrawMenu() {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float barH = 46.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - barH));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, barH));
    ImGui::Begin("##bar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.06f, 0.06f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.04f, 0.04f, 1.0f));
    static int tab = 0;
    const char *names[] = { u8"БОЙ", u8"ВИЗУАЛ", u8"РАЗНОЕ", u8"НАСТРОЙКИ" };
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(names[i], ImVec2(vp->Size.x / 4 - 12, barH - 12))) tab = i;
    }
    ImGui::PopStyleColor(3);
    ImGui::End();

    if (tab >= 0) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2 - 220, vp->Pos.y + vp->Size.y / 2 - 160), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440, 320), ImGuiCond_FirstUseEver);
        ImGui::Begin(u8"nh menu (host)", nullptr, ImGuiWindowFlags_NoCollapse);
        switch (tab) {
        case 0:
            ImGui::Checkbox(u8"aimbot", &b_aim);
            ImGui::Checkbox(u8"esp", &b_esp);
            break;
        case 1:
            ImGui::Checkbox(u8"esp box", &b_esp);
            ImGui::Checkbox(u8"esp name", &b_name);
            ImGui::Checkbox(u8"esp hp", &b_hp);
            break;
        case 2:
            ImGui::Checkbox(u8"speed", &b_speed);
            break;
        case 3:
            ImGui::SliderFloat(u8"alpha", &g_alpha, 0.2f, 1.0f);
            break;
        }
        ImGui::End();
    }
}

static DWORD WINAPI MenuThread(LPVOID) {
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = _T("nhmenu_host");
    RegisterClassEx(&wc);

    // sit on top of the emulator window
    g_hwnd = CreateWindowEx(WS_EX_TOPMOST, wc.lpszClassName, _T("nh menu"),
        WS_POPUP | WS_VISIBLE, 100, 100, 800, 520, nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(g_hwnd)) { CleanupDevice(); UnregisterClass(wc.lpszClassName, wc.hInstance); return 1; }

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dCtx);
    io.Fonts->AddFontDefault();

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, g_hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (GetAsyncKeyState(VK_END) & 1) g_running = false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawMenu();
        ImGui::Render();
        const float cc[] = { 0.06f, 0.06f, 0.08f, g_alpha };
        g_pd3dCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
        g_pd3dCtx->ClearRenderTargetView(g_pRTV, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(g_hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        HANDLE t = CreateThread(nullptr, 0, MenuThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
    }
    return TRUE;
}
