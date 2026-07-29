// video_player.h -- Bink video playback for the preview pane.
//
// GW2 ships its cinematics as Bink 2 streams inside Gw2.dat (magic "KB2i"/"KB2j",
// 1920x1080 @30, one audio track). They are decoded by the very same runtime the
// game uses: gw2_dat_dlls/bink2w64.dll (Bink 2.7p/1.3009), loaded lazily with
// GetProcAddress -- no import library, and the app still starts fine without it.
//
// Everything runs on the calling (UI) thread: pump() is non-blocking (it asks
// BinkWait whether the next frame is due yet) so a WM_TIMER can drive playback.
#ifndef GW2_VIDEO_PLAYER_H
#define GW2_VIDEO_PLAYER_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace castlemist::vid {

/// Header facts, parsed straight from the file -- no DLL needed, so the info
/// panel can describe a video even when the Bink runtime is missing.
struct Info {
    bool        ok = false;
    char        fourcc[5] = {0}; // "KB2j", "BIKi", ...
    std::string codec;           // "Bink 2 (KB2j)"
    uint32_t    width = 0;
    uint32_t    height = 0;
    uint32_t    frames = 0;
    uint32_t    fps_num = 0;
    uint32_t    fps_den = 1;
    uint32_t    tracks = 0;      // audio tracks
    uint32_t    largest_frame = 0;
    uint32_t    flags = 0;
    bool        has_alpha = false;
    double      seconds = 0.0;
    uint64_t    file_size = 0;
};

/// Cheap magic test ("KB2*" = Bink 2, "BIK*" = Bink 1).
bool is_video(const uint8_t* data, size_t size);

/// Parse the 44-byte Bink header. Returns false if it isn't a usable Bink file.
bool probe(const uint8_t* data, size_t size, Info& out);

// --- runtime (bink2w64.dll) ---------------------------------------------

/// Loads the DLL if it hasn't been loaded yet. Safe to call repeatedly; a failed
/// load is remembered so we don't retry on every entry click.
bool runtime_available();
/// Full path of the DLL we loaded (empty if none), and its FileVersion string.
const std::wstring& runtime_path();
const std::string&  runtime_version();
/// Human-readable reason the last open()/runtime load failed.
const std::string&  last_error();

// --- playback -----------------------------------------------------------

/// Opens `data` in place (Bink's BINKFROMMEMORY): the buffer is NOT copied and
/// must stay alive and unmoved until close(). Starts paused on frame 1.
bool open(const uint8_t* data, size_t size);
bool is_open();
void close();
/// Release the DLL + sound system (call on app shutdown).
void shutdown();

const Info& info();

/// Decodes the next frame if one is due. Returns true when a NEW frame landed in
/// frame_bgra() (the caller should upload it and repaint). Cheap no-op while
/// paused, closed, or when the frame's presentation time hasn't arrived yet.
bool pump();

/// Last decoded frame: `width * height` BGRA8 pixels, top-down, tightly packed.
const uint8_t* frame_bgra();
uint32_t       frame_pitch();

void play();
void pause();
void toggle_pause();
bool playing();       // open, not paused, not finished
void stop();          // pause and rewind to the first frame
bool finished();      // hit the last frame with looping off

void set_looping(bool on);
bool looping();

/// 0-based frame index of the frame currently in frame_bgra().
uint32_t current_frame();
double   position_seconds();
double   duration_seconds();
/// Seeks to a 0-based frame and decodes it immediately (keeps play/pause state).
/// EXACT, and therefore not free: GW2's cinematics have a single key frame, so a
/// seek replays the delta chain (~3 ms per frame replayed). Ask
/// seek_cost_estimate_ms() first and put up a wait cursor when it is large.
void     seek_frame(uint32_t frame);
void     seek_seconds(double seconds);
/// Predicted cost in milliseconds of seek_frame(frame) from the current position.
double   seek_cost_estimate_ms(uint32_t frame);

bool has_audio();
void set_volume(int percent); // 0..100
int  volume();
void set_muted(bool on);
bool muted();

// --- audio tracks -------------------------------------------------------
//
// A Bink can carry several sound tracks (a game typically uses that for one
// language per track, or dialogue split from music/SFX). open() enumerates them
// and opens ALL of them, so switching is instant: the selection is applied by
// per-track volume, not by reopening the movie.

/// @brief One audio track of a Bink movie.
struct TrackInfo {
    uint32_t id = 0;        // Bink track ID (what BinkSetSoundTrack/BinkSetVolume take)
    uint32_t type = 0;      // raw BinkGetTrackType value
    uint32_t max_size = 0;  // largest single-frame size for this track, bytes
    uint32_t frequency = 0; // Hz      } from BinkOpenTrack; 0 if it could not be
    uint32_t bits = 0;      // bits    } probed (the track is still selectable)
    uint32_t channels = 0;  // 1/2/... }
};

uint32_t         track_count();
const TrackInfo* track(uint32_t index); // nullptr if out of range
/// -1 (the default) mixes every track, as the game would play it; 0..n-1 makes
/// exactly that one track audible and silences the rest.
void set_active_track(int index);
int  active_track();
/// "Track 2/3 — 44100 Hz stereo" for the UI.
std::string track_label(uint32_t index);

} // namespace castlemist::vid

#endif // GW2_VIDEO_PLAYER_H
