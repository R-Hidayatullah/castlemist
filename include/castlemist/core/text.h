/// @file
/// @brief Narrow/wide conversion, byte-buffer sniffing and small display formatters.
/// @ingroup core

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::core {

/// @addtogroup core
/// @{

/// @brief Convert UTF-16 to the process ANSI code page (for `char` APIs and file paths).
std::string to_ansi(const std::wstring& wide);

/// @brief Widen a 7-bit ASCII string. Bytes >= 0x80 are copied verbatim.
std::wstring from_ascii(const std::string& ascii);

/// @brief Convert UTF-8 to UTF-16, falling back to the ANSI code page.
/// @param data      Raw bytes.
/// @param max_bytes Cap, so a 100 MB entry cannot be pushed into an edit control.
///
/// GW2 text entries are not tagged with an encoding. UTF-8 is tried strictly
/// (`MB_ERR_INVALID_CHARS`); only if that rejects the buffer is it re-read as
/// ANSI, which cannot fail and therefore must never be tried first.
std::wstring bytes_to_wide(const std::vector<uint8_t>& data, size_t max_bytes = 1u << 22);

/// @brief Heuristic: does @p data look like human-readable text rather than a binary blob?
///
/// Samples the first @p sample_bytes and requires >= 95% of them to be tab,
/// CR, LF, printable ASCII or >= 0x80. Deliberately permissive about high
/// bytes so UTF-8 and Latin-1 both pass.
bool looks_like_text(const std::vector<uint8_t>& data, size_t sample_bytes = 4096);

/// @brief Format @p seconds as `m:ss` for the audio/video transport labels.
std::wstring format_mmss(double seconds);

/// @brief Loudspeaker layout label for a channel count ("Mono", "Stereo", "5.1", ...).
const wchar_t* channel_layout_name(unsigned channels) noexcept;

/// @brief Format a byte count as `1.2 MB` / `345 KB` / `78 B`.
std::wstring format_bytes(uint64_t bytes);

/// @}

} // namespace castlemist::core
