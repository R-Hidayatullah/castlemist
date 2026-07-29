// audio_player.h -- decode a GW2 audio clip (MP3 via dr_mp3) and play it through
// the Windows waveOut API. Single-clip: starting a new clip stops the current one.
#ifndef GW2_AUDIO_PLAYER_H
#define GW2_AUDIO_PLAYER_H

#include <cstdint>
#include <cstddef>

namespace castlemist::snd {

/// @brief What a probe learned about a clip without playing it.
struct ClipInfo {
    bool          ok = false;
    unsigned      channels = 0;
    unsigned      sampleRate = 0;
    double        seconds = 0.0;
    const char*   codec = "unknown";
};

/// Decode just enough to report channels / sample rate / duration (no playback).
ClipInfo probe(const uint8_t* data, size_t size);

/// Decode `data` (an MP3/… audio file) and start playing it. Stops any current
/// playback first. Returns false if the audio could not be decoded.
bool play(const uint8_t* data, size_t size);

/// Stop playback and release the audio device.
void stop();

/// True while a clip is actively playing (auto-clears when it reaches the end).
bool is_playing();

/// Total duration of the clip decoded by the last play() call, in seconds (0 if
/// none). Available after play() even once playback has finished/stopped.
double duration_seconds();

/// Current playback head position in seconds (0 when idle). Clamped to duration.
double position_seconds();

/// Jump to `seconds` into the clip decoded by the last play() and resume playing
/// from there. Returns false if there is no decoded clip to seek. Used by the
/// preview seek bar (click/drag).
bool seek(double seconds);

/// Release the device on shutdown.
void shutdown();

} // namespace castlemist::snd

#endif // GW2_AUDIO_PLAYER_H
