// gw2host.cpp -- runs the Guild Wars 2 client as a library inside our own
// process, so an external previewer program owns the process and the game
// engine is just the renderer behind it.
//
// Flow:
//   1. LoadLibrary("Gw2-64.dll")      (produced by tools/mkgw2dll.py)
//      -> lands at its preferred base 0x140000000, so every address from the
//         IDA database is valid verbatim, no rebasing math anywhere.
//   2. install identity hooks         BEFORE a single line of game code runs
//   3. CreateThread -> Gw2RealEntry() (the original CRT entry -> WinMain)
//
// Step 2 is the whole reason for doing this instead of DLL injection: with
// injection the client has already booted by the time we arrive, so anything
// that has to be in place before startup (renderer capture, login bypass,
// content redirection) is a race. Here we are simply earlier than the game.
//
// The client is self-referential in a few places -- it asks the OS for "the
// process image" and expects to be told about itself. Since the process image
// is now gw2host.exe, three APIs are re-pointed at the game module:
//   GetModuleHandleW/A(NULL)   -> the game module, not the host
//   GetModuleFileNameW/A(NULL) -> the original Gw2-64.exe path
//   ExitProcess                -> park the engine thread, let the host decide
//
// Build:  ./build.sh        Run: place next to Gw2.dat and run gw2host.exe

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <psapi.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MinHook.h"

namespace {

// ---------------------------------------------------------------- logging --

FILE*         g_log = nullptr;
CRITICAL_SECTION g_log_lock;

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_lock);
    fputs(buf, stdout);
    fflush(stdout);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    LeaveCriticalSection(&g_log_lock);
    OutputDebugStringA(buf);
}

std::string narrow(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// ------------------------------------------------------------ host config --

struct Config {
    std::wstring game_dir;                 // where Gw2.dat lives
    std::wstring dll_path;                 // Gw2-64.dll
    std::wstring exe_identity;             // what GetModuleFileName(NULL) reports
    std::wstring gw2_args;                 // command line handed to the client
    bool         run_engine     = true;    // --dry loads and inspects only
    bool         identity_hooks = true;    // --no-identity leaves the APIs alone
    bool         exit_with_game = true;
};

Config      g_cfg;
HMODULE     g_gw2      = nullptr;   // the game module
uintptr_t   g_gw2_base = 0;
HANDLE      g_engine_thread = nullptr;
HANDLE      g_engine_done   = nullptr;
volatile LONG g_engine_exit_code = 0;

// ------------------------------------------------------ identity hooks -----
//
// These run on every thread in the process, including ours, so the host must
// finish resolving its own paths before MH_EnableHook.

using pGetModuleHandleW   = HMODULE (WINAPI*)(LPCWSTR);
using pGetModuleHandleA   = HMODULE (WINAPI*)(LPCSTR);
using pGetModuleFileNameW = DWORD   (WINAPI*)(HMODULE, LPWSTR, DWORD);
using pGetModuleFileNameA = DWORD   (WINAPI*)(HMODULE, LPSTR, DWORD);
using pGetCommandLineW    = LPWSTR  (WINAPI*)();
using pGetCommandLineA    = LPSTR   (WINAPI*)();
using pExitProcess        = void    (WINAPI*)(UINT);

pGetModuleHandleW   o_GetModuleHandleW   = nullptr;
pGetModuleHandleA   o_GetModuleHandleA   = nullptr;
pGetModuleFileNameW o_GetModuleFileNameW = nullptr;
pGetModuleFileNameA o_GetModuleFileNameA = nullptr;
pGetCommandLineW    o_GetCommandLineW    = nullptr;
pGetCommandLineA    o_GetCommandLineA    = nullptr;
pExitProcess        o_ExitProcess        = nullptr;

// The client parses its own argv (Param.cpp, 194 switches). It must never see
// the host's switches, so hand it a synthesised command line instead.
std::wstring g_gw2_cmdline_w;
std::string  g_gw2_cmdline_a;

LPWSTR WINAPI h_GetCommandLineW() { return (LPWSTR)g_gw2_cmdline_w.c_str(); }
LPSTR  WINAPI h_GetCommandLineA() { return (LPSTR)g_gw2_cmdline_a.c_str(); }

HMODULE WINAPI h_GetModuleHandleW(LPCWSTR name) {
    if (!name) return g_gw2;
    return o_GetModuleHandleW(name);
}
HMODULE WINAPI h_GetModuleHandleA(LPCSTR name) {
    if (!name) return g_gw2;
    return o_GetModuleHandleA(name);
}

DWORD WINAPI h_GetModuleFileNameW(HMODULE mod, LPWSTR out, DWORD cch) {
    if (mod == nullptr || mod == g_gw2) {
        const std::wstring& p = g_cfg.exe_identity;
        DWORD n = (DWORD)p.size();
        if (cch == 0) return 0;
        if (n >= cch) {
            memcpy(out, p.c_str(), (cch - 1) * sizeof(wchar_t));
            out[cch - 1] = 0;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return cch;
        }
        memcpy(out, p.c_str(), (n + 1) * sizeof(wchar_t));
        SetLastError(ERROR_SUCCESS);
        return n;
    }
    return o_GetModuleFileNameW(mod, out, cch);
}

DWORD WINAPI h_GetModuleFileNameA(HMODULE mod, LPSTR out, DWORD cch) {
    if (mod == nullptr || mod == g_gw2) {
        std::string p = narrow(g_cfg.exe_identity.c_str());
        DWORD n = (DWORD)p.size();
        if (cch == 0) return 0;
        if (n >= cch) {
            memcpy(out, p.c_str(), cch - 1);
            out[cch - 1] = 0;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return cch;
        }
        memcpy(out, p.c_str(), n + 1);
        SetLastError(ERROR_SUCCESS);
        return n;
    }
    return o_GetModuleFileNameA(mod, out, cch);
}

// The client calls ExitProcess when the player quits. That would take the host
// down with it, so the engine thread is parked instead and the host is told.
void WINAPI h_ExitProcess(UINT code) {
    if (GetCurrentThreadId() == GetThreadId(g_engine_thread)) {
        logf("[host] engine requested ExitProcess(%u) -- parking engine thread\n", code);
        InterlockedExchange(&g_engine_exit_code, (LONG)code);
        SetEvent(g_engine_done);
        ExitThread(code);
    }
    o_ExitProcess(code);
}

bool install_identity_hooks() {
    struct { LPVOID target; LPVOID detour; LPVOID* orig; const char* name; } hooks[] = {
        { (LPVOID)&GetModuleHandleW,   (LPVOID)&h_GetModuleHandleW,   (LPVOID*)&o_GetModuleHandleW,   "GetModuleHandleW"   },
        { (LPVOID)&GetModuleHandleA,   (LPVOID)&h_GetModuleHandleA,   (LPVOID*)&o_GetModuleHandleA,   "GetModuleHandleA"   },
        { (LPVOID)&GetModuleFileNameW, (LPVOID)&h_GetModuleFileNameW, (LPVOID*)&o_GetModuleFileNameW, "GetModuleFileNameW" },
        { (LPVOID)&GetModuleFileNameA, (LPVOID)&h_GetModuleFileNameA, (LPVOID*)&o_GetModuleFileNameA, "GetModuleFileNameA" },
        { (LPVOID)&GetCommandLineW,    (LPVOID)&h_GetCommandLineW,    (LPVOID*)&o_GetCommandLineW,    "GetCommandLineW"    },
        { (LPVOID)&GetCommandLineA,    (LPVOID)&h_GetCommandLineA,    (LPVOID*)&o_GetCommandLineA,    "GetCommandLineA"    },
        { (LPVOID)&ExitProcess,        (LPVOID)&h_ExitProcess,        (LPVOID*)&o_ExitProcess,        "ExitProcess"        },
    };
    for (auto& h : hooks) {
        MH_STATUS s = MH_CreateHook(h.target, h.detour, h.orig);
        if (s != MH_OK) {
            logf("[host] MH_CreateHook(%s) failed: %s\n", h.name, MH_StatusToString(s));
            return false;
        }
    }
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        logf("[host] MH_EnableHook failed: %s\n", MH_StatusToString(s));
        return false;
    }
    logf("[host] identity hooks armed (GetModuleHandle/FileName -> game module, ExitProcess trapped)\n");
    return true;
}

// ------------------------------------------------------------ engine boot --

using pGw2Entry = int (WINAPI*)();
pGw2Entry g_gw2_entry = nullptr;

DWORD WINAPI engine_thread(LPVOID) {
    logf("[host] engine thread %lu entering Gw2RealEntry @ %p\n",
         GetCurrentThreadId(), (void*)g_gw2_entry);
    // No __try here on purpose: MinGW's g++ has no MS-SEH keywords, and the
    // client installs its own unhandled-exception filter (the one that writes
    // Crash.dmp) as soon as it boots, which would take precedence anyway.
    int rc = g_gw2_entry();
    logf("[host] Gw2RealEntry returned %d\n", rc);
    InterlockedExchange(&g_engine_exit_code, rc);
    SetEvent(g_engine_done);
    return (DWORD)rc;
}

// ------------------------------------------------------------------ paths --

std::wstring dir_of(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : path.substr(0, p);
}

bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ------------------------------------------------------------ diagnostics --

void report_module(HMODULE m) {
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return;
    logf("[host] game module base %p  size %#lx  entry(hdr) %p\n",
         mi.lpBaseOfDll, mi.SizeOfImage, mi.EntryPoint);
    if ((uintptr_t)mi.lpBaseOfDll == 0x140000000ull) {
        logf("[host] base is the preferred 0x140000000 -- IDA addresses map 1:1\n");
    } else {
        logf("[host] WARNING: relocated. IDA address 0x140xxxxxx -> add slide %#llx\n",
             (unsigned long long)((uintptr_t)mi.lpBaseOfDll - 0x140000000ull));
    }
}

void usage() {
    printf(
        "gw2host -- run the Guild Wars 2 client as a library in this process\n"
        "\n"
        "  --gamedir <dir>    folder holding Gw2.dat  (default: folder of this exe)\n"
        "  --dll <path>       the converted client    (default: <gamedir>\\Gw2-64.dll)\n"
        "  --identity <path>  what GetModuleFileName(NULL) reports to the client\n"
        "                     (default: <gamedir>\\Gw2-64.exe)\n"
        "  --gw2-args \"...\"   command line handed to the client\n"
        "  --dry              load and inspect the module, do not start the engine\n"
        "  --no-identity      leave GetModuleHandle/FileName/ExitProcess alone\n"
        "  --keep-alive       keep the host running after the engine exits\n");
}

bool parse_args(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);   // before any hook is armed
    g_cfg.game_dir = dir_of(self);

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t* what) -> std::wstring {
            if (i + 1 >= argc) { logf("[host] %ls needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == L"--gamedir")     g_cfg.game_dir = next(L"--gamedir");
        else if (a == L"--dll")         g_cfg.dll_path = next(L"--dll");
        else if (a == L"--identity")    g_cfg.exe_identity = next(L"--identity");
        else if (a == L"--gw2-args")    g_cfg.gw2_args = next(L"--gw2-args");
        else if (a == L"--dry")         g_cfg.run_engine = false;
        else if (a == L"--no-identity") g_cfg.identity_hooks = false;
        else if (a == L"--keep-alive")  g_cfg.exit_with_game = false;
        else if (a == L"-h" || a == L"--help") { usage(); exit(0); }
        else { logf("[host] unknown argument: %ls\n", a.c_str()); usage(); return false; }
    }

    if (g_cfg.dll_path.empty())     g_cfg.dll_path     = g_cfg.game_dir + L"\\Gw2-64.dll";
    if (g_cfg.exe_identity.empty()) g_cfg.exe_identity = g_cfg.game_dir + L"\\Gw2-64.exe";

    g_gw2_cmdline_w = L"\"" + g_cfg.exe_identity + L"\"";
    if (!g_cfg.gw2_args.empty()) g_gw2_cmdline_w += L" " + g_cfg.gw2_args;
    g_gw2_cmdline_a = narrow(g_gw2_cmdline_w.c_str());
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    InitializeCriticalSection(&g_log_lock);
    g_log = _wfopen(L"gw2host.log", L"w");

    if (!parse_args(argc, argv)) return 2;

    logf("[host] host module %p, game dir %ls\n", GetModuleHandleW(nullptr), g_cfg.game_dir.c_str());
    logf("[host] loading %ls\n", g_cfg.dll_path.c_str());

    if (!file_exists(g_cfg.dll_path)) {
        logf("[host] not found. Produce it with:\n"
             "       python tools/mkgw2dll.py \"<gamedir>\\Gw2-64.exe\" \"%ls\"\n",
             g_cfg.dll_path.c_str());
        return 1;
    }

    // The client resolves Gw2.dat and its bin64 helpers relative to the current
    // directory in places, so run as if we had been started from the game dir.
    SetCurrentDirectoryW(g_cfg.game_dir.c_str());

    g_gw2 = LoadLibraryExW(g_cfg.dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_gw2) {
        logf("[host] LoadLibraryEx failed, GetLastError=%lu\n", GetLastError());
        return 1;
    }
    g_gw2_base = (uintptr_t)g_gw2;
    report_module(g_gw2);

    g_gw2_entry = (pGw2Entry)GetProcAddress(g_gw2, "Gw2RealEntry");
    if (!g_gw2_entry) {
        logf("[host] export Gw2RealEntry missing -- was the DLL built by mkgw2dll.py?\n");
        return 1;
    }
    logf("[host] Gw2RealEntry = %p (rva %#llx)\n",
         (void*)g_gw2_entry, (unsigned long long)((uintptr_t)g_gw2_entry - g_gw2_base));

    if (!g_cfg.run_engine) {
        logf("[host] --dry: module loaded and validated, engine not started\n");
        return 0;
    }

    if (MH_Initialize() != MH_OK) {
        logf("[host] MH_Initialize failed\n");
        return 1;
    }
    if (g_cfg.identity_hooks && !install_identity_hooks()) return 1;

    g_engine_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_engine_thread = CreateThread(nullptr, 0, engine_thread, nullptr, 0, nullptr);
    if (!g_engine_thread) {
        logf("[host] CreateThread failed, GetLastError=%lu\n", GetLastError());
        return 1;
    }

    logf("[host] engine running -- host owns the process\n");
    WaitForSingleObject(g_engine_done, INFINITE);
    logf("[host] engine finished, code %ld\n", g_engine_exit_code);

    if (!g_cfg.exit_with_game) {
        logf("[host] --keep-alive: press Ctrl+C to quit\n");
        for (;;) Sleep(1000);
    }
    return (int)g_engine_exit_code;
}
