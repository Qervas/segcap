// injector.cpp
//
// Loads segcap.dll into a target process via the classic
// CreateRemoteThread(LoadLibraryW) route.
//
// Deliberately the simplest thing that works. Every target in this project is
// a single-player title with no anti-tamper, so there is nothing to evade and
// no reason to reach for manual mapping or thread hijacking. Complexity here
// would buy nothing and cost debuggability.
//
// Usage:
//   injector.exe --pid 1234           --dll C:\path\to\segcap.dll
//   injector.exe --name Stray-Win64-Shipping.exe --dll ...
//   injector.exe --name d3d12_testapp.exe --dll ... --wait 30
//
//   --wait N   poll up to N seconds for the process to appear, then inject.
//              Lets the injector be started before the target.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[injector] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    std::fflush(stdout);
}

void LogLastError(const char* what) {
    const DWORD e = GetLastError();
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    Log("%s failed: %lu (%s)", what, e, msg ? msg : "?");
    if (msg) LocalFree(msg);
}

// Returns 0 if not found. Matches on executable name, case-insensitive.
DWORD FindProcessByName(const std::wstring& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool IsModuleLoaded(DWORD pid, const std::wstring& dllName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool Inject(DWORD pid, const std::wstring& dllPath) {
    // PROCESS_ALL_ACCESS is more than needed, but a partial access mask that
    // silently lacks one right produces a failure several calls later. Ask for
    // everything and fail immediately and legibly if we cannot have it.
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        LogLastError("OpenProcess");
        Log("hint: if the target runs elevated, this injector must too");
        return false;
    }

    bool ok = false;
    void* remote = nullptr;
    HANDLE thread = nullptr;

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
    if (!remote) {
        LogLastError("VirtualAllocEx");
        goto done;
    }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        LogLastError("WriteProcessMemory");
        goto done;
    }

    {
        // kernel32 is mapped at the same base in every process in a session,
        // so our LoadLibraryW address is valid in the target. This assumption
        // breaks across architectures -- injector and target must both be x64.
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<void*>(GetProcAddress(k32, "LoadLibraryW")));
        if (!loadLib) {
            LogLastError("GetProcAddress(LoadLibraryW)");
            goto done;
        }

        thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
        if (!thread) {
            LogLastError("CreateRemoteThread");
            goto done;
        }
    }

    if (WaitForSingleObject(thread, 15000) == WAIT_TIMEOUT) {
        Log("remote LoadLibraryW did not return within 15s");
        Log("hint: DllMain may be blocking -- do not do real work in DllMain");
        goto done;
    }

    {
        DWORD exitCode = 0;
        GetExitCodeThread(thread, &exitCode);
        // The thread's exit code is the low 32 bits of the returned HMODULE.
        // Zero means LoadLibraryW failed inside the target, which is usually a
        // missing dependency of our own DLL rather than an injection problem.
        if (exitCode == 0) {
            Log("LoadLibraryW returned NULL inside the target");
            Log("hint: a dependency of segcap.dll failed to resolve; the CRT is");
            Log("      statically linked precisely to avoid this");
            goto done;
        }
        Log("LoadLibraryW returned 0x%08lX (low 32 bits of HMODULE)", exitCode);
        ok = true;
    }

done:
    if (thread) CloseHandle(thread);
    // The remote allocation is intentionally leaked on success: freeing it
    // races the loader, which may still be reading the path string.
    if (remote && !ok) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

std::wstring Widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void Usage(const char* exe) {
    std::printf(
        "usage: %s (--pid N | --name EXE) --dll PATH [--wait SECONDS]\n"
        "\n"
        "  --pid N        target process id\n"
        "  --name EXE     target executable name, e.g. d3d12_testapp.exe\n"
        "  --dll PATH     DLL to inject (absolute path strongly preferred)\n"
        "  --wait S       poll up to S seconds for the process to appear\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::wstring name, dll;
    int waitSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = (i + 1 < argc);
        if (a == "--pid" && hasNext) pid = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--name" && hasNext) name = Widen(argv[++i]);
        else if (a == "--dll" && hasNext) dll = Widen(argv[++i]);
        else if (a == "--wait" && hasNext) waitSeconds = std::atoi(argv[++i]);
        else if (a == "--help") { Usage(argv[0]); return 0; }
        else { Log("unknown argument: %s", a.c_str()); Usage(argv[0]); return 2; }
    }

    if (dll.empty() || (pid == 0 && name.empty())) {
        Usage(argv[0]);
        return 2;
    }

    // Resolve to a full path here rather than in the target, where a relative
    // path would be interpreted against the game's working directory.
    {
        wchar_t full[MAX_PATH] = {};
        if (GetFullPathNameW(dll.c_str(), MAX_PATH, full, nullptr)) dll = full;
    }
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("DLL not found: %ls", dll.c_str());
        return 1;
    }
    Log("dll: %ls", dll.c_str());

    if (pid == 0) {
        const int deadline = waitSeconds > 0 ? waitSeconds : 0;
        for (int elapsed = 0;; ++elapsed) {
            pid = FindProcessByName(name);
            if (pid) break;
            if (elapsed >= deadline) {
                Log("process not found: %ls", name.c_str());
                return 1;
            }
            Sleep(1000);
        }
    }
    Log("target pid: %lu", pid);

    const std::wstring dllName = BaseName(dll);
    if (IsModuleLoaded(pid, dllName)) {
        Log("%ls is already loaded in the target; refusing to double-inject",
            dllName.c_str());
        return 0;
    }

    if (!Inject(pid, dll)) {
        Log("injection FAILED");
        return 1;
    }

    // Confirm rather than trust the return value. RenderDoc reported a
    // successful launch earlier in this project while nothing was actually
    // hooked, because the target relaunched itself through Steam. Verifying
    // the module is present is cheap and catches that class of lie.
    Sleep(300);
    if (IsModuleLoaded(pid, dllName)) {
        Log("VERIFIED: %ls is loaded in pid %lu", dllName.c_str(), pid);
        return 0;
    }
    Log("WARNING: LoadLibraryW succeeded but %ls is not in the module list",
        dllName.c_str());
    return 1;
}
