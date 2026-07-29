/// @file
/// @brief Implementation of the text/encoding helpers.

#include "castlemist/core/text.h"

#include <algorithm>
#include <cstdio>

#include <windows.h>

namespace castlemist::core {

std::string to_ansi(const std::wstring& wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string narrow(static_cast<size_t>(needed) - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, narrow.data(), needed, nullptr, nullptr);
    return narrow;
}

std::wstring from_ascii(const std::string& ascii) {
    std::wstring w;
    w.reserve(ascii.size());
    for (unsigned char c : ascii) w.push_back(static_cast<wchar_t>(c));
    return w;
}

std::wstring bytes_to_wide(const std::vector<uint8_t>& data, size_t max_bytes) {
    if (data.empty()) return {};
    int len = static_cast<int>(std::min<size_t>(data.size(), max_bytes));
    const char* p = reinterpret_cast<const char*>(data.data());

    UINT cp = CP_UTF8;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    if (need <= 0) {
        cp = CP_ACP;
        need = MultiByteToWideChar(CP_ACP, 0, p, len, nullptr, 0);
    }
    if (need <= 0) return {};

    std::wstring w(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, p, len, w.data(), need);
    return w;
}

bool looks_like_text(const std::vector<uint8_t>& data, size_t sample_bytes) {
    if (data.empty()) return false;
    size_t sample = std::min<size_t>(data.size(), sample_bytes);
    size_t printable = 0;
    for (size_t i = 0; i < sample; ++i) {
        uint8_t c = data[i];
        // Anything but a control byte other than tab/CR/LF counts as text.
        if (c == 0x09 || c == 0x0A || c == 0x0D || (c >= 0x20 && c <= 0x7E) || c >= 0x80) {
            ++printable;
        }
    }
    return printable * 100 >= sample * 95;
}

std::wstring format_mmss(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    int t = static_cast<int>(seconds + 0.5);
    wchar_t b[16];
    swprintf(b, 16, L"%d:%02d", t / 60, t % 60);
    return b;
}

const wchar_t* channel_layout_name(unsigned channels) noexcept {
    switch (channels) {
    case 0:  return L"?";
    case 1:  return L"Mono";
    case 2:  return L"Stereo";
    case 6:  return L"5.1";
    default: return L"multi-channel";
    }
}

std::wstring format_bytes(uint64_t bytes) {
    wchar_t b[48];
    if (bytes >= 1024ull * 1024 * 1024) {
        swprintf(b, 48, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        swprintf(b, 48, L"%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        swprintf(b, 48, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf(b, 48, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return b;
}

} // namespace castlemist::core
