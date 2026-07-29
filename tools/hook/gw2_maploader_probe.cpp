// gw2_maploader_probe.cpp
// Route A -- probe M1 (observasi) + M2 (paksa gerbang SERVER_WAIT) untuk
// menjadikan client GW2 map viewer offline.
//
// HASIL M1 (terverifikasi live, mapId 23):
//   1 LOAD_CONTENT -> 2 LOAD_MANIFEST -> 3 SERVER_WAIT -> [OnServerReady]
//   -> 5 -> 7 -> 9 -> 10 -> 11 AGENT_STREAM -> 12 READY_WAIT -> 13 READY
//   OnServerReady hanya menggerakkan state lewat SATU tulisan: loader+708 = 1.
//   a7 (encryption key) = NULL untuk map non-terenkripsi.
//   => M2 tidak perlu memalsukan 8 argumen; cukup +708 = 1 (+ spawn opsional).
//
// DUA KENDALA DESAIN (hasil RE, jangan diubah tanpa alasan):
//   [1] Konteks game THREAD-LOCAL (sub_1409B1760: gs:[0x58] -> TlsIndex -> +0x10).
//       Memanggil fungsi game dari thread DLL = sampah/crash. Semua aksi game
//       dijalankan dari hook PeekMessageW, yaitu thread pompa-pesan game.
//   [2] Hook CMapLoader::Update TIDAK berdetak di layar login (map loader belum
//       ditick). Karena itu tick hotkey memakai PeekMessageW, bukan Update.
//
// Build:  ./build_maploader.sh          Inject: inject.exe <dll> Gw2-64-disabled-aslr.exe
// Output: maploader_probe.log (shareable; bisa di-tail saat game jalan)

#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <share.h>
#include "MinHook.h"

static HMODULE   g_self = nullptr;
static uintptr_t g_base = 0;
static size_t    g_size = 0;
static FILE*     g_fp   = nullptr;
static wchar_t   g_dir[MAX_PATH] = {0};   // folder DLL, untuk pemicu berbasis file

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
    if (g_fp) { fputs(buf, g_fp); fflush(g_fp); }
}

// ---------------- signature scan ----------------
static uint8_t* find_sig(const char* sig) {
    std::vector<int> pat;
    for (const char* p = sig; *p; ) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { pat.push_back(-1); while (*p == '?') ++p; }
        else { pat.push_back((int)strtol(p, nullptr, 16)); p += 2; }
    }
    const size_t n = pat.size();
    if (!n || g_size < n) return nullptr;
    uint8_t* base = (uint8_t*)g_base;
    for (size_t i = 0; i + n <= g_size; ++i) {
        size_t j = 0;
        for (; j < n; ++j) if (pat[j] != -1 && base[i + j] != (uint8_t)pat[j]) break;
        if (j == n) return base + i;
    }
    return nullptr;
}
// resolve `lea/mov reg,[rip+disp32]`: disp32 ada di fn+off+opl, insn panjang off+opl+4
static uint8_t* rip_target(uint8_t* fn, size_t off, size_t oplen) {
    int32_t disp = *(int32_t*)(fn + off + oplen);
    return fn + off + oplen + 4 + disp;
}

// ---------------- signatures ----------------
static const char* SIG_UPDATE =        // sub_14094FB80  CMapLoader::Update
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 60 48 63 81";
static const char* SIG_ONSERVERREADY = // sub_140951E70  CMapLoader::OnServerReady
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 49 8B F8 48 8B F2 48 8B D9 E8 ? ? ? ? 44 8B 83";
static const char* SIG_LOCALRESOLVE =  // sub_14094DA60  VdfContext.cpp:2577
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 81 EC 80 07 00 00";
static const char* SIG_MAPTYPE =       // sub_1412DBBF0  MapTypeFromMapId
    "8B D1 48 8D 0D ? ? ? ? E9 ? ? ? ? CC CC 48 83 C1 08";
static const char* SIG_PARAMFLAG =     // sub_1410E1C70  ParamGetFlag
    "40 53 48 83 EC 20 8B D9 83 F9 7B";
static const char* SIG_GAMECTX =       // sub_1409B1760  GameContext (TLS)
    "8B 0D ? ? ? ? 65 48 8B 04 25 ? ? ? ? BA 10 00 00 00";

// ---------------- CMapLoader / CMapDef offsets ----------------
enum : size_t {
    OFF_MAPDEF = 696, OFF_STATE = 704, OFF_SERVER_FLAG = 708,
    OFF_SPAWN_XY = 680, OFF_SPAWN_Z = 688, OFF_LOADPARAM = 692,
    OFF_ASSET_PCT = 724, OFF_SERVER_TMO = 740,
    OFF_READY_TIMER = 768, OFF_DL_TIMER = 772, OFF_ENCKEY = 792,
};
enum : size_t { MAPDEF_MAPID = 40, MAPDEF_FLAGS = 152 };

static const char* state_name(int s) {
    switch (s) {
        case 0: return "IDLE";            case 1: return "LOAD_CONTENT";
        case 2: return "LOAD_MANIFEST";   case 3: return "SERVER_WAIT";
        case 4: return "ERROR";           case 5: return "MAP_DOWNLOAD";
        case 6: return "DOWNLOAD_ERROR";  case 7: return "MAP_STREAM";
        case 9: return "MODELS_STREAM";   case 10: return "MAP_ASSET_STREAM";
        case 11: return "AGENT_STREAM";   case 12: return "READY_WAIT";
        case 13: return "READY";          default: return "?";
    }
}

// ---------------- targets ----------------
typedef void (__fastcall *t_update)(void*, int);
typedef void (__fastcall *t_onready)(void*, void*, void*, void*, void*, float, void*, void*);
typedef void (__fastcall *t_localresolve)(void);
typedef BOOL (WINAPI *t_peek)(LPMSG, HWND, UINT, UINT, UINT);

static t_update       o_update       = nullptr;
static t_onready      o_onready      = nullptr;
static t_peek         o_peek         = nullptr;
static t_localresolve p_localresolve = nullptr;
static uint32_t*      p_paramflags   = nullptr;
static uint32_t       g_tlsIndex     = 0xFFFFFFFF;

// ---------------- game context (aman: cek null, tanpa memanggil fungsi game) ----------------
// Replika sub_1409B1760: gs:[0x58] -> tlsArray[TlsIndex] -> *(slot + 0x10)
static void* game_ctx() {
    if (g_tlsIndex == 0xFFFFFFFF) return nullptr;
    uint8_t** arr = (uint8_t**)__readgsqword(0x58);
    if (!arr) return nullptr;
    uint8_t* slot = arr[g_tlsIndex];
    if (!slot) return nullptr;                 // thread ini TIDAK punya konteks game
    return *(void**)(slot + 0x10);
}

// ---------------- crash logger (VEH) ----------------
// Tanpa ini, panggilan berisiko yang crash hanya meninggalkan log terpotong.
// Dengan ini kita dapat RVA faulting + stack, yang bisa dilacak balik di IDA.
static volatile LONG g_risky = 0;          // 1 = sedang di dalam panggilan berisiko
static volatile LONG g_local_inflight = 0; // 1 = LocalMapResolve dipanggil & belum kembali
static LONG          g_veh_logged = 0;

static LONG CALLBACK veh_handler(EXCEPTION_POINTERS* ep) {
    const DWORD c = ep->ExceptionRecord->ExceptionCode;
    const bool hard =
        c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
        c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_STACK_OVERFLOW ||
        c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_IN_PAGE_ERROR;
    // Di luar panggilan berisiko: hanya hard fault (game rutin melempar exception C++
    // yang ia tangani sendiri -- kalau semua dicatat, log tenggelam).
    // Selama g_risky: catat SEMUA, termasuk 0xE06D7363 (C++ EH) -- itulah hipotesis
    // kenapa LocalMapResolve tidak pernah kembali ke pemanggilnya.
    if (!hard && !g_risky) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_veh_logged) > 8) return EXCEPTION_CONTINUE_SEARCH;

    uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionAddress;
    bool in_mod = (uintptr_t)a >= g_base && (uintptr_t)a < g_base + g_size;
    logf("\n!!! EXCEPTION code=0x%08lX (%s) addr=%p%s  risky=%ld tid=%lu\n",
         c, hard ? "HARD FAULT" : (c == 0xE06D7363 ? "C++ throw" : "lain"),
         a, in_mod ? "" : " (di luar modul game)", g_risky, GetCurrentThreadId());
    if (in_mod) logf("    RVA = 0x%llX   -> IDA: 0x%llX\n",
                     (unsigned long long)((uintptr_t)a - g_base),
                     (unsigned long long)(0x140000000ull + ((uintptr_t)a - g_base)));
    if (c == EXCEPTION_ACCESS_VIOLATION)
        logf("    %s alamat %p\n",
             ep->ExceptionRecord->ExceptionInformation[0] ? "TULIS ke" : "BACA dari",
             (void*)ep->ExceptionRecord->ExceptionInformation[1]);

    void* fr[16]; USHORT n = RtlCaptureStackBackTrace(0, 16, fr, nullptr);
    logf("    stack:\n");
    for (USHORT i = 0; i < n; ++i) {
        uintptr_t f = (uintptr_t)fr[i];
        if (f >= g_base && f < g_base + g_size)
            logf("      [%2u] IDA 0x%llX\n", i, (unsigned long long)(0x140000000ull + (f - g_base)));
        else
            logf("      [%2u] %p (luar modul)\n", i, fr[i]);
    }
    logf("!!! /EXCEPTION\n\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------- state ----------------
static void*     g_loader      = nullptr;
static int       g_last_state  = -1;
static uint64_t  g_state_since = 0;
static uint64_t  g_update_hits = 0;
static DWORD     g_game_tid    = 0;
static volatile LONG g_req_report = 0, g_req_force = 0, g_req_local = 0;

static void dump_loader(void* L, const char* tag) {
    if (!L) { logf("[probe] %-10s (belum ada CMapLoader)\n", tag); return; }
    uint8_t* b = (uint8_t*)L;
    void* md = *(void**)(b + OFF_MAPDEF);
    uint32_t mid = md ? *(uint32_t*)((uint8_t*)md + MAPDEF_MAPID) : 0;
    uint32_t mfl = md ? *(uint32_t*)((uint8_t*)md + MAPDEF_FLAGS) : 0;
    logf("[probe] %-10s loader=%p mapDef=%p mapId=%u mapFlags=0x%08X%s\n"
         "        state=%d(%s) srvFlag=%u srvTmo=%d dlTmo=%d rdyTmr=%d assetPct=%.3f encKey=%p\n",
         tag, L, md, mid, mfl, (mfl & 0x20000000u) ? "  [ENCRYPTED]" : "",
         *(int*)(b + OFF_STATE), state_name(*(int*)(b + OFF_STATE)),
         *(uint32_t*)(b + OFF_SERVER_FLAG), *(int*)(b + OFF_SERVER_TMO),
         *(int*)(b + OFF_DL_TIMER), *(int*)(b + OFF_READY_TIMER),
         *(float*)(b + OFF_ASSET_PCT), *(void**)(b + OFF_ENCKEY));
}

// ---------------- detours ----------------
static void __fastcall hk_onready(void* L, void* a2, void* mapDef, void* a4,
                                  void* a5, float a6, void* a7, void* a8) {
    logf("\n=== OnServerReady (ASLI) === loader=%p a2=%p mapDef=%p a4=%p\n"
         "    a5=%p a6=%.6f a7(encKey)=%p a8=%p\n", L, a2, mapDef, a4, a5, a6, a7, a8);
    if (a5) { float* f = (float*)a5; logf("    spawn = (%.3f, %.3f, %.3f)\n", f[0], f[1], f[2]); }
    o_onready(L, a2, mapDef, a4, a5, a6, a7, a8);
    dump_loader(L, "post-ready");
    return;
}

static void __fastcall hk_update(void* L, int dt) {
    g_loader = L;
    ++g_update_hits;
    int st = *(int*)((uint8_t*)L + OFF_STATE);
    if (st != g_last_state) {
        uint64_t now = GetTickCount64();
        if (g_last_state >= 0)
            logf("[probe]   (%s bertahan %llu ms)\n", state_name(g_last_state),
                 (unsigned long long)(now - g_state_since));
        g_state_since = now;
        g_last_state = st;
        dump_loader(L, "STATE ->");
    }
    o_update(L, dt);
}

// Tick thread-game: berjalan juga di layar login. Semua aksi game dieksekusi di sini.
static BOOL WINAPI hk_peek(LPMSG m, HWND h, UINT a, UINT b, UINT c) {
    // Heartbeat sekali: membuktikan tick M2 benar-benar berdetak, dan di thread mana.
    // Kalau baris ini TIDAK muncul di log, PeekMessageW bukan pompa pesan game.
    static LONG once = 0;
    if (!InterlockedExchange(&once, 1))
        logf("[probe] tick PeekMessageW HIDUP (tid=%lu)\n", GetCurrentThreadId());

    // Kalau flag ini masih 1 di tick berikutnya, berarti LocalMapResolve TIDAK
    // kembali normal ke pemanggilnya -- stack-nya di-unwind oleh exception yang
    // ditangkap game. Deteksi ini bulletproof: tick berikutnya pasti terjadi.
    if (InterlockedExchange(&g_local_inflight, 0)) {
        InterlockedExchange(&g_risky, 0);
        logf("[M2] LocalMapResolve() TIDAK kembali normal -- stack di-unwind "
             "(exception ditangkap game). Game tetap hidup.\n");
    }

    if (InterlockedExchange(&g_req_report, 0)) {
        void* ctx = game_ctx();
        g_game_tid = GetCurrentThreadId();
        logf("\n--- LAPORAN PRASYARAT (tid=%lu) ---\n", g_game_tid);
        logf("    gameCtx           = %p %s\n", ctx, ctx ? "" : "  <- thread ini tanpa konteks TLS");
        if (ctx) {
            uint8_t* c8 = (uint8_t*)ctx;
            logf("    ctx+152 (world)   = %p\n", *(void**)(c8 + 152));
            logf("    ctx+216 (content) = %p\n", *(void**)(c8 + 216));
            logf("    ctx+224 (contMgr) = %p\n", *(void**)(c8 + 224));
            logf("    ctx+456 (missCli) = %p\n", *(void**)(c8 + 456));
        }
        logf("    Update dipanggil  = %llu kali\n", (unsigned long long)g_update_hits);
        logf("    paramFlag[49]     = %u\n", p_paramflags ? p_paramflags[49] : 0xFFFFFFFF);
        dump_loader(g_loader, "loader");
        logf("--- /LAPORAN ---\n\n");
    }

    if (InterlockedExchange(&g_req_force, 0)) {
        if (!g_loader) logf("[M2] tidak ada CMapLoader -- tidak ada yang bisa dipaksa.\n");
        else {
            uint8_t* L = (uint8_t*)g_loader;
            int st = *(int*)(L + OFF_STATE);
            if (st != 3) {
                logf("[M2] state=%d(%s), bukan SERVER_WAIT. Dibatalkan "
                     "(paksa hanya sah di state 3).\n", st, state_name(st));
            } else {
                logf("[M2] memaksa gerbang: loader+708 0 -> 1 (state 3 SERVER_WAIT)\n");
                *(uint32_t*)(L + OFF_SERVER_FLAG) = 1;
                dump_loader(g_loader, "post-force");
            }
        }
    }

    if (InterlockedExchange(&g_req_local, 0)) {
        void* ctx = game_ctx();
        if (!ctx) {
            logf("[M2] LocalMapResolve DIBATALKAN: thread ini tanpa konteks game.\n");
        } else if (!p_paramflags || !p_localresolve) {
            logf("[M2] LocalMapResolve DIBATALKAN: simbol belum ter-resolve.\n");
        } else {
            int st = g_loader ? *(int*)((uint8_t*)g_loader + OFF_STATE) : -1;
            if (g_loader && st != 0) {
                logf("[M2] LocalMapResolve DIBATALKAN: map loader sedang sibuk "
                     "(state=%d %s). Panggil hanya saat IDLE / belum ada map.\n", st, state_name(st));
            } else {
                logf("[M2] paramFlag[49] %u -> 1, memanggil LocalMapResolve() "
                     "(loaderState=%d) ...\n", p_paramflags[49], st);
                p_paramflags[49] = 1;
                InterlockedExchange(&g_risky, 1);
                InterlockedExchange(&g_local_inflight, 1);
                p_localresolve();
                InterlockedExchange(&g_local_inflight, 0);
                InterlockedExchange(&g_risky, 0);
                logf("[M2] LocalMapResolve() KEMBALI NORMAL.\n");
            }
        }
    }

    return o_peek(m, h, a, b, c);
}

// ---------------- hotkey watcher (tidak pernah memanggil fungsi game) ----------------
static DWORD WINAPI HotkeyThread(LPVOID) {
    bool p7 = false, p8 = false, p9 = false, p10 = false;
    for (;;) {
        bool n7  = (GetAsyncKeyState(VK_F7)  & 0x8000) != 0;
        bool n8  = (GetAsyncKeyState(VK_F8)  & 0x8000) != 0;
        bool n9  = (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
        bool n10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (n7  && !p7)  InterlockedExchange(&g_req_report, 1);
        if (n8  && !p8)  InterlockedExchange(&g_req_force,  1);
        if (n9  && !p9)  InterlockedExchange(&g_req_local,  1);
        if (n10 && !p10) dump_loader(g_loader, "F10 dump");
        p7 = n7; p8 = n8; p9 = n9; p10 = n10;
        Sleep(40);
    }
}

// ---------------- init ----------------
static DWORD WINAPI Init(LPVOID) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)hMod;
    auto nt  = (IMAGE_NT_HEADERS*)((uint8_t*)hMod + dos->e_lfanew);
    g_base = (uintptr_t)hMod;
    g_size = nt->OptionalHeader.SizeOfImage;

    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_self, path, MAX_PATH);
    if (wchar_t* s = wcsrchr(path, L'\\')) s[1] = 0;
    wcscat_s(path, L"maploader_probe.log");
    g_fp = _wfsopen(path, L"a", _SH_DENYNO);

    logf("\n########## gw2_maploader_probe (M1+M2) ##########\n");
    logf("[probe] base=%p size=%zu\n", (void*)g_base, g_size);

    uint8_t* pUpdate = find_sig(SIG_UPDATE);
    uint8_t* pReady  = find_sig(SIG_ONSERVERREADY);
    uint8_t* pLocal  = find_sig(SIG_LOCALRESOLVE);
    uint8_t* pMapTy  = find_sig(SIG_MAPTYPE);
    uint8_t* pParam  = find_sig(SIG_PARAMFLAG);
    uint8_t* pCtx    = find_sig(SIG_GAMECTX);
    logf("[probe] Update=%p OnServerReady=%p LocalResolve=%p MapType=%p ParamGetFlag=%p GameCtx=%p\n",
         pUpdate, pReady, pLocal, pMapTy, pParam, pCtx);

    p_localresolve = (t_localresolve)pLocal;

    if (pParam && pParam[0x26] == 0x48 && pParam[0x27] == 0x8D) {
        p_paramflags = (uint32_t*)rip_target(pParam, 0x26, 3);   // 48 8D 0D <disp32>
        logf("[probe] s_flags = %p  [flag49=%u]\n", (void*)p_paramflags, p_paramflags[49]);
    } else logf("[probe] s_flags TIDAK ter-resolve\n");

    if (pCtx && pCtx[0] == 0x8B && pCtx[1] == 0x0D) {
        uint32_t* pTls = (uint32_t*)rip_target(pCtx, 0, 2);      // 8B 0D <disp32>
        g_tlsIndex = *pTls;
        logf("[probe] TlsIndex global=%p -> %u\n", (void*)pTls, g_tlsIndex);
    } else logf("[probe] TlsIndex TIDAK ter-resolve\n");

    AddVectoredExceptionHandler(1, veh_handler);   // pertama: crash jadi data, bukan log terpotong

    if (MH_Initialize() != MH_OK) { logf("[probe] MH_Initialize gagal\n"); return 1; }

    if (pUpdate && MH_CreateHook(pUpdate, (void*)hk_update, (void**)&o_update) == MH_OK)
        MH_EnableHook(pUpdate); else logf("[probe] hook Update GAGAL\n");
    if (pReady && MH_CreateHook(pReady, (void*)hk_onready, (void**)&o_onready) == MH_OK)
        MH_EnableHook(pReady); else logf("[probe] hook OnServerReady GAGAL\n");

    void* pPeek = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "PeekMessageW");
    if (pPeek && MH_CreateHook(pPeek, (void*)hk_peek, (void**)&o_peek) == MH_OK)
        MH_EnableHook(pPeek); else logf("[probe] hook PeekMessageW GAGAL (tick M2 mati)\n");

    CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    logf("[probe] siap.  F7=laporan prasyarat  F8=paksa +708=1  F9=LocalMapResolve  F10=dump\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
