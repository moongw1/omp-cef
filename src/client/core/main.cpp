#include <windows.h>
#include <memory>

#include "runtime.hpp"

static std::unique_ptr<Runtime> runtime;

DWORD WINAPI AppInitializationThread(LPVOID)
{
	runtime = Runtime::CreateDefault();
    runtime->Start();
	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        HANDLE hThread = CreateThread(nullptr, 0, AppInitializationThread, nullptr, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        }
        else {
            MessageBoxA(nullptr, "Failed to create initialization thread.", "Error", MB_ICONERROR);
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (runtime) {
            if (reserved != nullptr) {
                // The process is already terminating. Do not run Runtime::Stop()
                // from DllMain under the loader lock: CEF/hook teardown can wait
                // on threads or APIs that need that same lock and leave gta_sa.exe
                // hidden but still alive after /q or /quit.
                //
                // The OS is tearing down the entire process, so deliberately
                // release ownership and let process termination reclaim memory.
                (void)runtime.release();
            }
            else {
                // Explicit DLL unload (FreeLibrary): keep the existing cleanup
                // path so hooks are not left pointing into an unloaded module.
                runtime.reset();
            }
        }
    }

    return TRUE;
}
