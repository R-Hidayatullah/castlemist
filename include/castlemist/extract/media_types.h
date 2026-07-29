/// @file
/// @brief Audio clips, cinematic subtitles and Bink video metadata.
/// @ingroup extract

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @addtogroup extract
/// @{

/// @brief One decoded sound.
///
/// An ASND entry or a raw `asnd` blob yields a single clip; an ABNK bank and an
/// AMSP sound script yield many. @ref data is always a *complete* audio file, so
/// it can be handed straight to `castlemist::snd::play` or `castlemist::snd::probe`.
struct AudioClipCPU {
    std::string codec;         ///< "MP3", "Ogg Vorbis", ...
    std::vector<uint8_t> data; ///< A complete, self-contained audio file.
};

/// @brief One dialogue line of a cinematic, from the CINP/CSCN timeline.
///
/// Carries a real start *and* end, the way an `.srt` cue does, rather than the
/// single trigger time the game fires it from.
struct SubtitleLine {
    double       time = 0.0;     ///< Shown from, in seconds into the sequence.
    double       end_time = 0.0; ///< ...until here.
    int          sequence = 0;   ///< Which CSCN sequence (i.e. which movie) it belongs to.
    std::wstring text;           ///< The line itself.
};

/// @brief Bink cinematic header facts, read during extraction.
///
/// Read by castlemist's own header parser so the info panel can describe a
/// movie even when the Bink runtime is missing. Mirrors `castlemist::vid::Info`, kept as
/// a plain struct so this header stays free of the Bink SDK.
struct VideoMeta {
    bool        ok = false;   ///< The header parsed.
    std::string fourcc;       ///< "KB2i", "KB2j", "BIKi", ...
    std::string codec;        ///< Human label, e.g. "Bink 2 (KB2j)".
    uint32_t    width = 0;    ///< Pixels.
    uint32_t    height = 0;   ///< Pixels.
    uint32_t    frames = 0;   ///< Frame count.
    uint32_t    fps_num = 0;  ///< Frame rate numerator.
    uint32_t    fps_den = 1;  ///< Frame rate denominator.
    uint32_t    tracks = 0;   ///< Audio track count.
    bool        has_alpha = false; ///< Movie carries an alpha plane.
    double      seconds = 0.0;     ///< Duration, `frames * fps_den / fps_num`.
};

/// @}
