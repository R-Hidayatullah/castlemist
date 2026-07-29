// video_player.cpp -- Bink playback via GW2's own bink2w64.dll.
//
// Old headers + new runtime. The declarations come from the Bink 2.7d SDK
// (external/bink2-2.7d/bink_2_7d.h, 2017); the code that actually decodes is
// bink2w64.dll 2.7p/1.3009 (2019) as shipped inside Gw2.dat. That pairing is
// deliberate: the 2.7d *library* predates the "KB2i"/"KB2j" stream revisions GW2
// uses today, but the API/ABI never moved inside the 2.7 series, so the old
// headers describe the new DLL exactly (sizeof(BINK) == 1744 for both).
//
// Verified against real dat entries before this file was written:
//   baseId 611275 (KB2i, 1920x1080, 2674 frames) and 745724/646122/645137 (KB2j)
//   all open, seek and decode; 90 frames of KB2j played in 3.00 s wall clock
//   (exactly 30 fps) at ~6.1 ms decode + ~2.3 ms BGRA convert per 1080p frame.
//
// Nothing is linked against Bink: every entry point is resolved by name, so a
// missing/mismatched DLL degrades to "no video preview" instead of a load failure.

#include "castlemist/media/video_player.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <windows.h>

#define __RADINSTATICLIB__ 1 // RADEXPFUNC -> nothing; we never import, only GetProcAddress
#include "bink_2_7d.h"

#pragma comment(lib, "version.lib")

namespace castlemist::vid {
namespace {

// ---------------------------------------------------------------- DLL binding

typedef HBINK (RADEXPLINK* PFN_BinkOpen)(char const*, U32);
typedef void  (RADEXPLINK* PFN_BinkClose)(HBINK);
typedef S32   (RADEXPLINK* PFN_BinkDoFrame)(HBINK);
typedef void  (RADEXPLINK* PFN_BinkNextFrame)(HBINK);
typedef S32   (RADEXPLINK* PFN_BinkWait)(HBINK);
typedef S32   (RADEXPLINK* PFN_BinkPause)(HBINK, S32);
typedef S32   (RADEXPLINK* PFN_BinkCopyToBuffer)(HBINK, void*, S32, U32, U32, U32, U32);
typedef void  (RADEXPLINK* PFN_BinkGoto)(HBINK, U32, S32);
typedef char* (RADEXPLINK* PFN_BinkGetError)(void);
typedef S32   (RADEXPLINK* PFN_BinkSetSoundSystem)(void*, UINTa);
typedef void* (RADEXPLINK* PFN_BinkOpenWaveOut)(UINTa);
typedef void* (RADEXPLINK* PFN_BinkOpenDirectSound)(UINTa);
typedef void* (RADEXPLINK* PFN_BinkOpenXAudio2)(UINTa, UINTa);
typedef S32   (RADEXPLINK* PFN_BinkSetSoundTrack)(U32, U32*);
typedef void  (RADEXPLINK* PFN_BinkSetVolume)(HBINK, U32, S32);
typedef S32   (RADEXPLINK* PFN_BinkSetSoundOnOff)(HBINK, S32);
typedef void  (RADEXPLINK* PFN_BinkSetWillLoop)(HBINK, S32);
typedef U32   (RADEXPLINK* PFN_BinkGetTrackID)(HBINK, U32);
typedef U32   (RADEXPLINK* PFN_BinkGetTrackType)(HBINK, U32);
typedef U32   (RADEXPLINK* PFN_BinkGetTrackMaxSize)(HBINK, U32);
typedef HBINKTRACK (RADEXPLINK* PFN_BinkOpenTrack)(HBINK, U32);
typedef void  (RADEXPLINK* PFN_BinkCloseTrack)(HBINKTRACK);
typedef void  (RADEXPLINK* PFN_BinkFreeGlobals)(void);

struct Api {
    PFN_BinkOpen            Open = nullptr;
    PFN_BinkClose           Close = nullptr;
    PFN_BinkDoFrame         DoFrame = nullptr;
    PFN_BinkNextFrame       NextFrame = nullptr;
    PFN_BinkWait            Wait = nullptr;
    PFN_BinkPause           Pause = nullptr;
    PFN_BinkCopyToBuffer    CopyToBuffer = nullptr;
    PFN_BinkGoto            Goto = nullptr;
    PFN_BinkGetError        GetError = nullptr;
    PFN_BinkSetSoundSystem  SetSoundSystem = nullptr;
    PFN_BinkOpenWaveOut     OpenWaveOut = nullptr;
    PFN_BinkOpenDirectSound OpenDirectSound = nullptr;
    PFN_BinkOpenXAudio2     OpenXAudio2 = nullptr;
    PFN_BinkSetSoundTrack   SetSoundTrack = nullptr;
    PFN_BinkSetVolume       SetVolume = nullptr;
    PFN_BinkSetSoundOnOff   SetSoundOnOff = nullptr;
    PFN_BinkSetWillLoop     SetWillLoop = nullptr;
    PFN_BinkGetTrackID      GetTrackID = nullptr;
    PFN_BinkGetTrackType    GetTrackType = nullptr;
    PFN_BinkGetTrackMaxSize GetTrackMaxSize = nullptr;
    PFN_BinkOpenTrack       OpenTrack = nullptr;
    PFN_BinkCloseTrack      CloseTrack = nullptr;
    PFN_BinkFreeGlobals     FreeGlobals = nullptr;
};

HMODULE      g_dll = nullptr;
bool         g_dll_tried = false;
Api          g_api;
std::wstring g_dll_path;
std::string  g_dll_version;
std::string  g_error;
bool         g_sound_ready = false;
const char*  g_sound_system = "none";

// Bink's own volume scale: 0 .. 32768 (full).
constexpr int kBinkVolumeMax = 32768;

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

// Reads FileVersion out of the DLL's version resource, purely so the info panel
// can state which Bink build is doing the decoding.
std::string file_version(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (sz == 0) {
        return {};
    }
    std::vector<uint8_t> blob(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, blob.data())) {
        return {};
    }
    struct LangCp { WORD lang, cp; } * lc = nullptr;
    UINT lc_len = 0;
    if (!VerQueryValueW(blob.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&lc), &lc_len) ||
        lc_len < sizeof(LangCp)) {
        return {};
    }
    wchar_t key[128];
    swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\FileVersion", lc->lang, lc->cp);
    wchar_t* val = nullptr;
    UINT val_len = 0;
    if (!VerQueryValueW(blob.data(), key, reinterpret_cast<void**>(&val), &val_len) || val_len == 0) {
        return {};
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, val, -1, nullptr, 0, nullptr, nullptr);
    std::string out(need > 0 ? need - 1 : 0, '\0');
    if (need > 1) {
        WideCharToMultiByte(CP_UTF8, 0, val, -1, out.data(), need, nullptr, nullptr);
    }
    return out;
}

bool bind_all() {
#define BIND(field, name)                                                              \
    g_api.field = reinterpret_cast<PFN_##name>(GetProcAddress(g_dll, #name));          \
    if (g_api.field == nullptr) {                                                      \
        g_error = "bink2w64.dll is missing the export " #name;                         \
        return false;                                                                  \
    }
    BIND(Open, BinkOpen)
    BIND(Close, BinkClose)
    BIND(DoFrame, BinkDoFrame)
    BIND(NextFrame, BinkNextFrame)
    BIND(Wait, BinkWait)
    BIND(Pause, BinkPause)
    BIND(CopyToBuffer, BinkCopyToBuffer)
    BIND(Goto, BinkGoto)
    BIND(GetError, BinkGetError)
    BIND(SetSoundSystem, BinkSetSoundSystem)
    BIND(SetSoundTrack, BinkSetSoundTrack)
    BIND(SetVolume, BinkSetVolume)
    BIND(SetSoundOnOff, BinkSetSoundOnOff)
    BIND(SetWillLoop, BinkSetWillLoop)
    BIND(GetTrackID, BinkGetTrackID)
#undef BIND
    // Optional: the sound back-ends and FreeGlobals. Missing ones just mean the
    // corresponding capability is unavailable, not a failed load.
    g_api.GetTrackType = reinterpret_cast<PFN_BinkGetTrackType>(GetProcAddress(g_dll, "BinkGetTrackType"));
    g_api.GetTrackMaxSize = reinterpret_cast<PFN_BinkGetTrackMaxSize>(GetProcAddress(g_dll, "BinkGetTrackMaxSize"));
    g_api.OpenTrack = reinterpret_cast<PFN_BinkOpenTrack>(GetProcAddress(g_dll, "BinkOpenTrack"));
    g_api.CloseTrack = reinterpret_cast<PFN_BinkCloseTrack>(GetProcAddress(g_dll, "BinkCloseTrack"));
    g_api.OpenWaveOut = reinterpret_cast<PFN_BinkOpenWaveOut>(GetProcAddress(g_dll, "BinkOpenWaveOut"));
    g_api.OpenDirectSound = reinterpret_cast<PFN_BinkOpenDirectSound>(GetProcAddress(g_dll, "BinkOpenDirectSound"));
    g_api.OpenXAudio2 = reinterpret_cast<PFN_BinkOpenXAudio2>(GetProcAddress(g_dll, "BinkOpenXAudio2"));
    g_api.FreeGlobals = reinterpret_cast<PFN_BinkFreeGlobals>(GetProcAddress(g_dll, "BinkFreeGlobals"));
    return true;
}

// Where to look for the runtime, in priority order. The DLL is not shipped: it
// is extracted from Gw2.dat (BINARIES 'MZx', baseId 140117) into
// dumps/binaries/, which is why the search climbs out of build/<preset>/bin/
// towards the repository root. GW2_BINK_DLL overrides everything.
bool load_runtime() {
    if (g_dll_tried) {
        return g_dll != nullptr;
    }
    g_dll_tried = true;

    std::vector<std::wstring> candidates;
    wchar_t env[MAX_PATH * 2] = {0};
    if (GetEnvironmentVariableW(L"GW2_BINK_DLL", env, static_cast<DWORD>(std::size(env))) > 0) {
        candidates.emplace_back(env);
    }
    const std::wstring dir = exe_dir();
    const wchar_t* rel[] = {
        L"bink2w64.dll",
        L"dumps\\binaries\\bink2w64.dll",
        L"..\\dumps\\binaries\\bink2w64.dll",
        L"..\\..\\dumps\\binaries\\bink2w64.dll",
        L"..\\..\\..\\dumps\\binaries\\bink2w64.dll",
    };
    for (const wchar_t* r : rel) {
        candidates.push_back(dir + r);
    }
    for (const wchar_t* r : rel) {
        candidates.emplace_back(r); // also relative to the working directory
    }

    for (const std::wstring& c : candidates) {
        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        HMODULE m = LoadLibraryW(c.c_str());
        if (m == nullptr) {
            continue;
        }
        g_dll = m;
        wchar_t full[MAX_PATH * 2] = {0};
        DWORD n = GetModuleFileNameW(m, full, static_cast<DWORD>(std::size(full)));
        g_dll_path.assign(full, n);
        g_dll_version = file_version(g_dll_path);
        if (!bind_all()) {
            FreeLibrary(m);
            g_dll = nullptr;
            g_dll_path.clear();
            g_dll_version.clear();
            return false;
        }
        return true;
    }
    g_error = "bink2w64.dll not found (looked next to castlemist.exe and in dumps/binaries/). "
              "Set GW2_BINK_DLL to its full path.";
    return false;
}

// Registers a sound back-end once, best available first. Video still plays if
// they all fail -- we just won't have audio.
void ensure_sound_system() {
    if (g_sound_ready || g_dll == nullptr) {
        return;
    }
    g_sound_ready = true;
    if (g_api.OpenXAudio2 && g_api.SetSoundSystem(reinterpret_cast<void*>(g_api.OpenXAudio2), 0)) {
        g_sound_system = "XAudio2";
    } else if (g_api.OpenDirectSound &&
               g_api.SetSoundSystem(reinterpret_cast<void*>(g_api.OpenDirectSound), 0)) {
        g_sound_system = "DirectSound";
    } else if (g_api.OpenWaveOut && g_api.SetSoundSystem(reinterpret_cast<void*>(g_api.OpenWaveOut), 0)) {
        g_sound_system = "WaveOut";
    } else {
        g_sound_system = "none";
    }
}

// ------------------------------------------------------------------- playback

HBINK                g_bink = nullptr;
Info                 g_info;
std::vector<uint8_t> g_frame; // BGRA, width*height*4
bool                 g_paused = true;
bool                 g_finished = false;
bool                 g_looping = false;
bool                 g_muted = false;
int                  g_volume = 100;
uint32_t             g_shown_frame = 0; // 0-based index of what's in g_frame
std::vector<TrackInfo> g_tracks;        // every sound track this movie carries
int                  g_active_track = -1; // -1 = mix all (as the game plays it)

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Volume is also how track selection is expressed: every track was opened, so
// making one audible is just a matter of zeroing the others. That keeps track
// switching instant -- BinkSetSoundTrack only takes effect at BinkOpen, so the
// alternative would be reopening the movie and replaying the seek.
void apply_volume() {
    if (g_bink == nullptr || g_api.SetVolume == nullptr || g_tracks.empty()) {
        return;
    }
    int v = g_muted ? 0 : (kBinkVolumeMax * std::clamp(g_volume, 0, 100)) / 100;
    const bool solo_is_voice =
        g_active_track >= 0 && static_cast<size_t>(g_active_track) < g_tracks.size() &&
        g_tracks[g_active_track].id != 0;
    for (size_t i = 0; i < g_tracks.size(); ++i) {
        bool audible;
        if (g_active_track < 0) {
            audible = true; // "All tracks (mixed)" -- every language at once
        } else if (static_cast<size_t>(g_active_track) == i) {
            audible = true;
        } else {
            // Soloing a VOICE track keeps track id 0 audible, because that is
            // exactly what the game does: BinkSetSoundTrack(2, {0, language+1}).
            // Track 0 is the shared music/SFX bed, not a language.
            audible = solo_is_voice && g_tracks[i].id == 0;
        }
        g_api.SetVolume(g_bink, g_tracks[i].id, audible ? v : 0);
    }
}

// Probes every track's id/type/format. Runs on a throwaway video-only open,
// because the track IDs are only readable from an opened BINK yet are exactly
// what BinkSetSoundTrack needs BEFORE the real open.
void probe_tracks(const uint8_t* data, uint32_t track_hint) {
    g_tracks.clear();
    if (track_hint == 0 || g_api.Open == nullptr) {
        return;
    }
    HBINK probe = g_api.Open(reinterpret_cast<char const*>(data), BINKFROMMEMORY);
    if (probe == nullptr) {
        // Fall back to assuming ids 0..n-1 so a multi-track file is still usable.
        for (uint32_t i = 0; i < track_hint; ++i) {
            g_tracks.push_back(TrackInfo{i, 0, 0, 0, 0, 0});
        }
        return;
    }
    uint32_t n = probe->NumTracks < 0 ? 0 : static_cast<uint32_t>(probe->NumTracks);
    for (uint32_t i = 0; i < n; ++i) {
        TrackInfo t;
        t.id = g_api.GetTrackID ? g_api.GetTrackID(probe, i) : i;
        t.type = g_api.GetTrackType ? g_api.GetTrackType(probe, i) : 0;
        t.max_size = g_api.GetTrackMaxSize ? g_api.GetTrackMaxSize(probe, i) : 0;
        if (g_api.OpenTrack && g_api.CloseTrack) {
            if (HBINKTRACK ht = g_api.OpenTrack(probe, i)) {
                // *** The one place the 2.7d header and the 2.7p runtime disagree. ***
                // The header declares BINKTRACK as {Frequency, Bits, Channels, MaxSize},
                // but dumping the real struct shows {Frequency, Channels, bufferSize, 0}:
                //   [0]=48000 [1]=2 [2]=154624 [3]=0   (GW2 cinematic, 48 kHz stereo)
                // Read via ht->Bits/ht->Channels you get "Bits=2, Channels=154624".
                // So take the words positionally and don't trust the field names here.
                // Bink 2 always decodes to 16-bit PCM, so Bits is simply gone.
                const uint32_t* w = reinterpret_cast<const uint32_t*>(ht);
                t.frequency = w[0];
                t.channels = w[1];
                t.bits = 16;
                if (t.channels == 0 || t.channels > 8) {
                    t.channels = 0; // don't report a value we clearly misread
                }
                g_api.CloseTrack(ht);
            }
        }
        g_tracks.push_back(t);
    }
    g_api.Close(probe);
}

// Decodes whatever frame Bink is currently pointing at into g_frame.
void decode_current() {
    if (g_bink == nullptr) {
        return;
    }
    g_api.DoFrame(g_bink);
    g_api.CopyToBuffer(g_bink, g_frame.data(), static_cast<S32>(g_info.width * 4), g_info.height, 0, 0,
                       g_info.has_alpha ? BINKSURFACE32BGRA : BINKSURFACE32BGRx);
    if (!g_info.has_alpha) {
        // 32BGRx leaves the 4th byte undefined; the preview shader may be in
        // alpha-aware mode, so force opaque rather than depend on it.
        uint8_t* p = g_frame.data();
        for (size_t i = 3; i < g_frame.size(); i += 4) {
            p[i] = 0xFF;
        }
    }
    g_shown_frame = g_bink->FrameNum > 0 ? g_bink->FrameNum - 1 : 0;
}

} // namespace

// ------------------------------------------------------------------ detection

bool is_video(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 44) {
        return false;
    }
    // Bink 2: "KB2" + revision letter. Bink 1: "BIK" + revision letter.
    const bool bink2 = data[0] == 'K' && data[1] == 'B' && data[2] == '2';
    const bool bink1 = data[0] == 'B' && data[1] == 'I' && data[2] == 'K';
    if (!bink2 && !bink1) {
        return false;
    }
    return data[3] >= 'a' && data[3] <= 'z';
}

bool probe(const uint8_t* data, size_t size, Info& out) {
    out = Info{};
    if (!is_video(data, size)) {
        return false;
    }
    std::memcpy(out.fourcc, data, 4);
    out.fourcc[4] = '\0';
    out.file_size = static_cast<uint64_t>(rd32(data + 4)) + 8;
    out.frames = rd32(data + 8);
    out.largest_frame = rd32(data + 12);
    out.width = rd32(data + 20);
    out.height = rd32(data + 24);
    out.fps_num = rd32(data + 28);
    out.fps_den = rd32(data + 32);
    out.flags = rd32(data + 36);
    out.tracks = rd32(data + 40);

    if (out.width == 0 || out.height == 0 || out.frames == 0 || out.fps_num == 0 ||
        out.width > 16384 || out.height > 16384) {
        return false;
    }
    if (out.fps_den == 0) {
        out.fps_den = 1;
    }
    out.has_alpha = (out.flags & BINKALPHA) != 0;
    out.seconds = static_cast<double>(out.frames) * out.fps_den / out.fps_num;
    out.codec = std::string(data[0] == 'K' ? "Bink 2 (" : "Bink 1 (") + out.fourcc + ")";
    out.ok = true;
    return true;
}

// -------------------------------------------------------------------- runtime

bool runtime_available() {
    return load_runtime();
}

const std::wstring& runtime_path() {
    return g_dll_path;
}

const std::string& runtime_version() {
    return g_dll_version;
}

const std::string& last_error() {
    return g_error;
}

// ------------------------------------------------------------------- playback

bool open(const uint8_t* data, size_t size) {
    close();

    Info probed;
    if (!probe(data, size, probed)) {
        g_error = "not a Bink video";
        return false;
    }
    if (!load_runtime()) {
        return false; // g_error already set
    }
    ensure_sound_system();

    U32 flags = BINKFROMMEMORY;
    std::vector<U32> ids;
    if (probed.tracks > 0 && std::strcmp(g_sound_system, "none") != 0) {
        probe_tracks(data, probed.tracks);
        // Open EVERY track, then let apply_volume() pick which one is audible.
        for (const TrackInfo& t : g_tracks) {
            ids.push_back(t.id);
        }
        if (!ids.empty()) {
            g_api.SetSoundTrack(static_cast<U32>(ids.size()), ids.data());
            flags |= BINKSNDTRACK;
        }
    } else {
        g_tracks.clear();
    }
    // Default to how the game itself plays the movie: the common bed plus ONE
    // voice track. Mixing every track would play all languages simultaneously.
    g_active_track = -1;
    for (size_t i = 0; i < g_tracks.size(); ++i) {
        if (g_tracks[i].id != 0) {
            g_active_track = static_cast<int>(i);
            break;
        }
    }
    if (probed.has_alpha) {
        flags |= BINKALPHA;
    }

    // BINKFROMMEMORY treats `name` as a pointer to the whole file image; Bink
    // reads it in place, which is exactly what we want for an already-
    // decompressed dat entry (no temp file, no second 100 MB copy).
    HBINK b = g_api.Open(reinterpret_cast<char const*>(data), flags);
    if (b == nullptr) {
        const char* e = g_api.GetError ? g_api.GetError() : nullptr;
        g_error = std::string("BinkOpen failed: ") + ((e && *e) ? e : "unknown error");
        return false;
    }

    g_bink = b;
    g_info = probed;
    // Trust the opened stream over the raw header for the fields Bink normalizes.
    g_info.width = b->Width;
    g_info.height = b->Height;
    g_info.frames = b->Frames;
    g_info.fps_num = b->FrameRate;
    g_info.fps_den = b->FrameRateDiv ? b->FrameRateDiv : 1;
    g_info.tracks = static_cast<uint32_t>(b->NumTracks < 0 ? 0 : b->NumTracks);
    g_info.seconds = static_cast<double>(g_info.frames) * g_info.fps_den / g_info.fps_num;

    g_frame.assign(static_cast<size_t>(g_info.width) * g_info.height * 4, 0);
    g_paused = true;
    g_finished = false;
    g_shown_frame = 0;

    if (g_api.SetWillLoop) {
        g_api.SetWillLoop(b, g_looping ? 1 : 0);
    }
    g_info.tracks = static_cast<uint32_t>(g_tracks.size() ? g_tracks.size() : g_info.tracks);
    apply_volume();
    g_api.Pause(b, 1); // hold on frame 1 until the user presses Play
    decode_current();  // ...but show that first frame right away
    return true;
}

bool is_open() {
    return g_bink != nullptr;
}

void close() {
    if (g_bink != nullptr) {
        g_api.Close(g_bink);
        g_bink = nullptr;
    }
    g_frame.clear();
    g_frame.shrink_to_fit();
    g_info = Info{};
    g_tracks.clear();
    g_paused = true;
    g_finished = false;
    g_shown_frame = 0;
}

void shutdown() {
    close();
    if (g_dll != nullptr) {
        if (g_api.FreeGlobals) {
            g_api.FreeGlobals(); // stops Bink's own worker/sound threads
        }
        FreeLibrary(g_dll);
        g_dll = nullptr;
    }
    g_api = Api{};
    g_sound_ready = false;
    g_sound_system = "none";
    g_dll_tried = false;
}

const Info& info() {
    return g_info;
}

bool pump() {
    if (g_bink == nullptr || g_paused || g_finished) {
        return false;
    }
    // BinkWait is the movie's clock: non-zero means "the next frame isn't due
    // yet", so we return immediately and let the timer call us again.
    if (g_api.Wait(g_bink)) {
        return false;
    }

    const bool last = g_bink->FrameNum >= g_bink->Frames;
    decode_current();

    if (last && !g_looping) {
        g_finished = true;
        g_paused = true;
        g_api.Pause(g_bink, 1);
    } else {
        g_api.NextFrame(g_bink); // wraps back to frame 1 on its own when looping
    }
    return true;
}

const uint8_t* frame_bgra() {
    return g_frame.empty() ? nullptr : g_frame.data();
}

uint32_t frame_pitch() {
    return g_info.width * 4;
}

void play() {
    if (g_bink == nullptr) {
        return;
    }
    if (g_finished) {
        seek_frame(0); // replay from the top
    }
    g_paused = false;
    g_finished = false;
    g_api.Pause(g_bink, 0);
}

void pause() {
    if (g_bink == nullptr) {
        return;
    }
    g_paused = true;
    g_api.Pause(g_bink, 1);
}

void toggle_pause() {
    if (g_paused) {
        play();
    } else {
        pause();
    }
}

bool playing() {
    return g_bink != nullptr && !g_paused && !g_finished;
}

void stop() {
    if (g_bink == nullptr) {
        return;
    }
    pause();
    seek_frame(0);
}

bool finished() {
    return g_finished;
}

void set_looping(bool on) {
    g_looping = on;
    if (g_bink != nullptr && g_api.SetWillLoop) {
        g_api.SetWillLoop(g_bink, on ? 1 : 0);
    }
}

bool looping() {
    return g_looping;
}

uint32_t current_frame() {
    return g_shown_frame;
}

double position_seconds() {
    if (g_info.fps_num == 0) {
        return 0.0;
    }
    return static_cast<double>(g_shown_frame) * g_info.fps_den / g_info.fps_num;
}

double duration_seconds() {
    return g_info.seconds;
}

// Cost of replaying one frame of the delta chain, measured on these 1080p
// cinematics (~3.2 ms). Only used to predict how slow a seek will be so the UI
// can warn/put up a wait cursor -- never to change what is decoded.
constexpr double kReplayMsPerFrame = 3.2;

double seek_cost_estimate_ms(uint32_t frame) {
    if (g_bink == nullptr || g_info.frames == 0) {
        return 0.0;
    }
    uint32_t f = std::min(frame, g_info.frames - 1);
    uint32_t cur = g_bink->FrameNum > 0 ? g_bink->FrameNum - 1 : 0;
    uint32_t replay = (f >= cur) ? (f - cur) : f; // forward delta, else from the top
    return replay * kReplayMsPerFrame;
}

void seek_frame(uint32_t frame) {
    if (g_bink == nullptr || g_info.frames == 0) {
        return;
    }
    uint32_t f = std::min(frame, g_info.frames - 1);
    uint32_t cur = g_bink->FrameNum > 0 ? g_bink->FrameNum - 1 : 0;

    // ArenaNet encodes these cinematics with a SINGLE key frame -- frame 1;
    // BinkGetKeyFrame(n, BINKGETKEYPREVIOUS) answers 1 for every n. So the only
    // way to land on a correct picture is to replay the delta chain.
    //
    // BINKGOTOQUICK was tried and REJECTED: it costs ~5 ms but produces a frame
    // reconstructed against whatever was in the buffer, which for these files is
    // visibly a different scene entirely (verified by pixel-diffing against a
    // full decode -- 47% of pixels wrong, and 30-120 catch-up frames only got it
    // to 7-15% wrong, still obvious garbage). Exactness wins; this is also what
    // the reference Bink player in ../archive-viewer does.
    //
    // The one honest saving: seeking FORWARD only has to decode the frames in
    // between, so short steps stay cheap. A backward seek must restart at the
    // key frame. Callers should use seek_cost_estimate_ms() to put up a wait
    // cursor -- a jump to the end of a 4700-frame movie really does take ~15 s.
    if (f >= cur) {
        while (g_bink->FrameNum < f + 1) {
            g_api.DoFrame(g_bink);
            g_api.NextFrame(g_bink);
        }
    } else {
        g_api.Goto(g_bink, f + 1, 0); // full, exact replay from the key frame
    }

    g_finished = false;
    decode_current();
    if (!g_paused) {
        g_api.Pause(g_bink, 0);
    }
}

void seek_seconds(double seconds) {
    if (g_info.fps_den == 0) {
        return;
    }
    double f = seconds * g_info.fps_num / g_info.fps_den;
    seek_frame(f <= 0 ? 0 : static_cast<uint32_t>(f));
}

bool has_audio() {
    return !g_tracks.empty() && std::strcmp(g_sound_system, "none") != 0;
}

uint32_t track_count() {
    return static_cast<uint32_t>(g_tracks.size());
}

const TrackInfo* track(uint32_t index) {
    return index < g_tracks.size() ? &g_tracks[index] : nullptr;
}

void set_active_track(int index) {
    g_active_track = (index < 0 || index >= static_cast<int>(g_tracks.size())) ? -1 : index;
    apply_volume();
}

int active_track() {
    return g_active_track;
}

// What a Bink track ID means in GW2, decoded from Video.cpp (sub_140D78170):
//
//     BinkSetSoundTrack(2, { 0, language + 1 });
//     BinkOpen(name, BINKIOPROCESSOR|BINKALPHA|BINKSNDTRACK|BINKNOFRAMEBUFFERS);
//
// i.e. the game always plays track id 0 (the shared music/SFX bed) plus exactly
// one voice track, chosen as `language + 1`. Confirmed against the dat: the four
// multi-track cinematics carry ids {0,1,3,4,6} / {1,3,4,6} -- one common track
// and one voice track per shipped language, with gaps where a language has no
// dub. That is why the 1080p movies the player usually meets sound like they
// have "no voice": those are single-track (id 0 only).
//
// The language index is therefore `id - 1`, named by GW2's GameLanguage enum
// (the same one entry_extractor.cpp's gw2_language_name() already uses for the
// text packs). Observed on the four multi-track cinematics: ids 1/3/4/6 =
// English, French, German, Chinese -- Korean (id 2) and Spanish (id 5) have no
// dub in those movies, which is exactly why the id is sparse.
const char* language_hint(uint32_t track_id) {
    switch (track_id) {
    case 0: return "common (music/SFX)";
    case 1: return "English";
    case 2: return "Korean";
    case 3: return "French";
    case 4: return "German";
    case 5: return "Spanish";
    case 6: return "Chinese";
    default: return "voice";
    }
}

std::string track_label(uint32_t index) {
    if (index >= g_tracks.size()) {
        return "All tracks";
    }
    const TrackInfo& t = g_tracks[index];
    std::string s = "id " + std::to_string(t.id) + " — " + language_hint(t.id);
    if (t.frequency) {
        s += " — " + std::to_string(t.frequency) + " Hz";
        if (t.channels == 1) s += " mono";
        else if (t.channels == 2) s += " stereo";
        else if (t.channels == 6) s += " 5.1";
        else if (t.channels) s += " " + std::to_string(t.channels) + " ch";
    }
    return s;
}

void set_volume(int percent) {
    g_volume = std::clamp(percent, 0, 100);
    apply_volume();
}

int volume() {
    return g_volume;
}

void set_muted(bool on) {
    g_muted = on;
    apply_volume();
}

bool muted() {
    return g_muted;
}

} // namespace castlemist::vid
