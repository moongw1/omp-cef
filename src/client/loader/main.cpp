#include <Windows.h>
#include <d3d9.h>
#include <filesystem>

#include <MinHook.h>

namespace
{
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(
        IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
        D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

    Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;
    CreateDeviceFn g_originalCreateDevice = nullptr;
    bool g_createDeviceHookInstalled = false;

    struct WindowState
    {
        HWND hwnd = nullptr;
        LONG_PTR style = 0;
        LONG_PTR exStyle = 0;
        RECT rect{};
        bool valid = false;
    };

    WindowState MakeBorderless(HWND hwnd, D3DPRESENT_PARAMETERS& pp)
    {
        WindowState old{};
        if (!hwnd || !IsWindow(hwnd))
            return old;

        old.hwnd = hwnd;
        old.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        old.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        GetWindowRect(hwnd, &old.rect);
        old.valid = true;

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(monitor, &mi))
            return old;

        const LONG_PTR borderlessStyle =
            (old.style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                           WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP | WS_VISIBLE;

        const LONG_PTR borderlessExStyle =
            old.exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                            WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_TOPMOST);

        SetWindowLongPtrW(hwnd, GWL_STYLE, borderlessStyle);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, borderlessExStyle);

        const int width = mi.rcMonitor.right - mi.rcMonitor.left;
        const int height = mi.rcMonitor.bottom - mi.rcMonitor.top;

        SetWindowPos(
            hwnd,
            HWND_NOTOPMOST,
            mi.rcMonitor.left,
            mi.rcMonitor.top,
            width,
            height,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

        pp.Windowed = TRUE;
        pp.hDeviceWindow = hwnd;
        pp.FullScreen_RefreshRateInHz = 0;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth = static_cast<UINT>(width);
        pp.BackBufferHeight = static_cast<UINT>(height);

        return old;
    }

    void RestoreWindow(const WindowState& old)
    {
        if (!old.valid || !old.hwnd || !IsWindow(old.hwnd))
            return;

        SetWindowLongPtrW(old.hwnd, GWL_STYLE, old.style);
        SetWindowLongPtrW(old.hwnd, GWL_EXSTYLE, old.exStyle);
        SetWindowPos(
            old.hwnd,
            nullptr,
            old.rect.left,
            old.rect.top,
            old.rect.right - old.rect.left,
            old.rect.bottom - old.rect.top,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }

    HRESULT STDMETHODCALLTYPE HookCreateDevice(
        IDirect3D9* self,
        UINT adapter,
        D3DDEVTYPE deviceType,
        HWND focusWindow,
        DWORD behaviorFlags,
        D3DPRESENT_PARAMETERS* params,
        IDirect3DDevice9** device)
    {
        if (!g_originalCreateDevice || !params)
            return D3DERR_INVALIDCALL;

        // Keep genuine windowed helper/dummy devices untouched. Only replace
        // exclusive fullscreen requests made by GTA with borderless windowed.
        if (params->Windowed)
        {
            return g_originalCreateDevice(
                self, adapter, deviceType, focusWindow, behaviorFlags, params, device);
        }

        const D3DPRESENT_PARAMETERS originalParams = *params;
        const HWND gameWindow = params->hDeviceWindow ? params->hDeviceWindow : focusWindow;
        WindowState oldWindow = MakeBorderless(gameWindow, *params);

        HRESULT hr = g_originalCreateDevice(
            self, adapter, deviceType, focusWindow, behaviorFlags, params, device);

        if (SUCCEEDED(hr))
            return hr;

        // Safety fallback for unusual D3D wrappers/drivers: if the converted
        // parameters are rejected, retry the exact fullscreen request so the
        // CEF loader can never prevent GTA from starting.
        RestoreWindow(oldWindow);
        *params = originalParams;
        return g_originalCreateDevice(
            self, adapter, deviceType, focusWindow, behaviorFlags, params, device);
    }

    void InstallCreateDeviceHook(IDirect3D9* d3d)
    {
        if (!d3d || g_createDeviceHookInstalled)
            return;

        void** vtable = *reinterpret_cast<void***>(d3d);
        if (!vtable || !vtable[16])
            return;

        if (MH_CreateHook(
                vtable[16],
                reinterpret_cast<void*>(&HookCreateDevice),
                reinterpret_cast<void**>(&g_originalCreateDevice)) != MH_OK)
            return;

        if (MH_EnableHook(vtable[16]) != MH_OK)
        {
            MH_RemoveHook(vtable[16]);
            g_originalCreateDevice = nullptr;
            return;
        }

        g_createDeviceHookInstalled = true;
    }

    IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
    {
        if (!g_originalDirect3DCreate9)
            return nullptr;

        IDirect3D9* d3d = g_originalDirect3DCreate9(sdkVersion);
        InstallCreateDeviceHook(d3d);
        return d3d;
    }

    void InitializeEarlyBorderlessHook()
    {
        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return;

        HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
        if (!d3d9)
            return;

        auto* target = reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"));
        if (!target)
            return;

        if (MH_CreateHook(
                target,
                reinterpret_cast<void*>(&HookDirect3DCreate9),
                reinterpret_cast<void**>(&g_originalDirect3DCreate9)) != MH_OK)
            return;

        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_originalDirect3DCreate9 = nullptr;
        }
    }
}

std::wstring GetErrorMessage(DWORD errorCode)
{
    if (errorCode == 0) {
        return L"No error message generated.";
    }

    LPWSTR messageBuffer = nullptr;
    size_t size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&messageBuffer,
        0,
        NULL);

    std::wstring message(messageBuffer, size);
    LocalFree(messageBuffer);
    return message;
}

void LoadCefClientDll()
{
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return;

    std::filesystem::path path(exePath);
    path = path.parent_path();

    std::filesystem::path cefPath = path / L"cef";

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(cefPath.wstring().c_str());

    std::filesystem::path clientDllPath = cefPath / L"client.dll";

    HMODULE clientModule = LoadLibraryW(clientDllPath.wstring().c_str());
    if (!clientModule)
    {
        DWORD err = GetLastError();
        std::wstring errMsg = GetErrorMessage(err);

        std::wstring fullMessage = L"Failed to load cef/client.dll\n\n";
        fullMessage += L"Reason: " + errMsg;
        fullMessage += L"(Error Code: " + std::to_wstring(err) + L")\n\n";
        fullMessage += L"Please ensure all required files (like libcef.dll, renderer.exe, etc.) are present and that your antivirus is not blocking the file.";

        MessageBoxW(nullptr, fullMessage.c_str(), L"omp-cef - Fatal Error", MB_ICONERROR);
        ExitProcess(0);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        // cef.asi is loaded early by the ASI loader, before GTA creates its
        // primary D3D9 device. Install the fullscreen conversion hook first.
        InitializeEarlyBorderlessHook();
        LoadCefClientDll();
    }

    return TRUE;
}
