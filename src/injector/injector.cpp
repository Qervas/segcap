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
#include <vector>

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

// Launch a process suspended, inject, then resume.
//
// This exists because descriptor->resource mapping can only be built by
// observing CreateRenderTargetView / CreateDepthStencilView. Views created
// before we are injected are unrecoverable -- there is no API to enumerate
// existing descriptors. Injecting into an already-running process therefore
// leaves permanent blind spots: against the test fixture, every single
// OMSetRenderTargets lookup missed.
//
// Starting suspended guarantees we are present before the device exists.
bool LaunchSuspendedAndInject(const std::wstring& exe, const std::wstring& args,
                              const std::wstring& dll, const std::wstring& workDir) {
    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!args.empty()) cmdline += L" " + args;

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // CreateProcessW may modify the command line buffer, so it cannot be const.
    std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(L'\0');

    if (!CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        LogLastError("CreateProcessW");
        return false;
    }

    Log("launched suspended: pid %lu", pi.dwProcessId);

    // Created BEFORE injection so the DLL can always find it. The DLL signals
    // this once its hooks are live. Resuming without waiting makes suspended
    // launch pointless -- LoadLibraryW returns as soon as DllMain returns, and
    // DllMain only spawns the init thread.
    wchar_t evName[64];
    _snwprintf_s(evName, _TRUNCATE, L"Local\\segcap_hooks_ready_%lu", pi.dwProcessId);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, evName);

    const bool ok = Inject(pi.dwProcessId, dll);
    if (!ok) {
        Log("injection failed; terminating the suspended process rather than");
        Log("resuming it un-instrumented and pretending that worked");
        TerminateProcess(pi.hProcess, 1);
    } else {
        if (ready) {
            Log("waiting for hooks to come up before resuming...");
            const DWORD w = WaitForSingleObject(ready, 30000);
            if (w == WAIT_OBJECT_0) {
                Log("hooks reported ready");
            } else {
                Log("WARNING: timed out waiting for hooks; resuming anyway.");
                Log("         descriptors created before hooks are live cannot");
                Log("         be recovered, so expect misses in the log");
            }
        }
        Log("resuming main thread");
        ResumeThread(pi.hThread);
    }
    if (ready) CloseHandle(ready);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

// Poll hard for a process to appear, then inject immediately.
//
// This exists because Steam launches Stray.exe (a shim) and the actual renderer
// is its child, Stray-Win64-Shipping.exe. Wrapping the launch with %command%
// would put our DLL in the shim, which renders nothing. And a plain suspended
// launch of the Shipping exe is defeated by SteamAPI_RestartAppIfNecessary --
// the game asks Steam to relaunch it and exits, which is exactly how RenderDoc
// silently ended up hooked to nothing earlier in this project.
//
// Polling at 5ms wins the race in practice: a UE4 title loads hundreds of DLLs
// before it creates a D3D12 device, so there is a wide window between "process
// exists" and "first CreateDepthStencilView". Whether we actually won is
// measurable rather than assumed -- segcap logs descriptor misses, and a
// nonzero count means we were late.
//
// Deliberately NOT suspending the target's threads first: CreateRemoteThread +
// LoadLibraryW needs the loader lock, and suspending a thread that already
// holds it deadlocks the process.
DWORD WaitForProcessAndInject(const std::wstring& name, const std::wstring& dll,
                              int timeoutSeconds) {
    Log("watching for %ls (timeout %ds)...", name.c_str(), timeoutSeconds);
    Log("launch the game now; injection happens the instant its process appears");

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutSeconds) * 1000;
    DWORD pid = 0;
    while (GetTickCount64() < deadline) {
        pid = FindProcessByName(name);
        if (pid) break;
        Sleep(5);
    }

    if (!pid) {
        Log("timed out; %ls never appeared", name.c_str());
        return 0;
    }

    Log("detected %ls as pid %lu -- injecting immediately", name.c_str(), pid);
    if (!Inject(pid, dll)) {
        Log("injection failed");
        return 0;
    }
    return pid;
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
        "usage: %s (--pid N | --name EXE | --launch EXE [ARGS...]) --dll PATH\n"
        "\n"
        "  --pid N         attach to a running process id\n"
        "  --name EXE      attach to a running process by executable name\n"
        "  --launch EXE    start EXE suspended, inject, then resume.\n"
        "                  PREFERRED: attaching to an already-running process\n"
        "                  misses every descriptor created before injection,\n"
        "                  and those cannot be recovered afterwards.\n"
        "  --watch EXE     poll for EXE and inject the instant it appears.\n"
        "                  Use this for Steam titles: start the injector first,\n"
        "                  then launch the game normally. Steam runs a shim whose\n"
        "                  child is the real renderer, and the game may relaunch\n"
        "                  itself via Steam, so neither --launch nor a %%command%%\n"
        "                  wrapper reliably lands on the right process.\n"
        "  --args \"...\"    arguments passed to the --launch target\n"
        "  --workdir DIR   working directory for --launch\n"
        "  --dll PATH      DLL to inject (absolute path strongly preferred)\n"
        "  --wait S        poll up to S seconds for the process to appear\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::wstring name, dll, launchExe, launchArgs, workDir, watchName;
    int waitSeconds = 120;  // only used by --watch; generous by default

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = (i + 1 < argc);
        if (a == "--pid" && hasNext) pid = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--name" && hasNext) name = Widen(argv[++i]);
        else if (a == "--dll" && hasNext) dll = Widen(argv[++i]);
        else if (a == "--wait" && hasNext) waitSeconds = std::atoi(argv[++i]);
        else if (a == "--workdir" && hasNext) workDir = Widen(argv[++i]);
        // --launch takes ONLY the executable, and target arguments go in
        // --args. An earlier version consumed every remaining token after
        // --launch, which silently swallowed --dll and produced a usage dump
        // instead of an error naming the real problem.
        else if (a == "--launch" && hasNext) launchExe = Widen(argv[++i]);
        else if (a == "--args" && hasNext) launchArgs = Widen(argv[++i]);
        else if (a == "--watch" && hasNext) watchName = Widen(argv[++i]);
        else if (a == "--help") { Usage(argv[0]); return 0; }
        else { Log("unknown argument: %s", a.c_str()); Usage(argv[0]); return 2; }
    }

    if (dll.empty() || (pid == 0 && name.empty() && launchExe.empty() && watchName.empty())) {
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

    if (!launchExe.empty()) {
        return LaunchSuspendedAndInject(launchExe, launchArgs, dll, workDir) ? 0 : 1;
    }

    if (!watchName.empty()) {
        const DWORD hit = WaitForProcessAndInject(watchName, dll, waitSeconds);
        if (!hit) return 1;
        Sleep(400);
        if (IsModuleLoaded(hit, BaseName(dll))) {
            Log("VERIFIED: %ls loaded in pid %lu", BaseName(dll).c_str(), hit);
            return 0;
        }
        Log("WARNING: LoadLibraryW succeeded but the module is not listed");
        return 1;
    }

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
