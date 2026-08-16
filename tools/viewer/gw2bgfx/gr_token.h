/// @file
/// @brief ArenaNet `Token` -- a packed lowercase string, not a hash.
///
/// `Token_Decode32` @ 0x140E46CF0, `Arena\Services\Token\Token.cpp:30`.
/// Alphabet at 0x1420B5738.
///
/// This is the single most load-bearing fact about GW2's material binding, and
/// it is easy to lose a week to: every id that looks like a name hash --
/// MODL material-constant names, engine-global shader params -- is a
/// **reversible base-23 encoding of the name itself**. Hashing uniform names
/// with Murmur/crc/fnv to match them never works, because there is no hash
/// involved.
///
/// (The bgfx *uniform registry* is a different matter and genuinely does hash:
/// `bx::HashMurmur2A` seed 0 over the uniform name, per `BgfxShaderD3D11_Create`.
/// Two id namespaces, two mechanisms; do not cross them.)

#pragma once

#include <cstdint>
#include <string>

namespace gw2bgfx {

/// @brief Token_Base23Alphabet @ 0x1420B5738.
///
/// 23 characters, no `j`, `q` or `z`. Note the order past `o`: `p v r s t u w x y`
/// -- `v` sits where `q` would be. Transcribing this alphabetically is a
/// silent corruption that decodes most tokens to plausible-looking nonsense.
inline constexpr const char kTokenBase23Alphabet[] = "abcdefghiklmnopvrstuwxy";

/// @brief `Token::Decode` -- 32-bit token to lowercase name.
///
/// @verbatim
///   v = (token - 0x30000000) mod 2^32
///   while (v) { out += ALPHA[v % 23]; v /= 23; }
/// @endverbatim
///
/// 23^7 just exceeds 2^32, so every decoded name is at most 7 characters --
/// which is why the engine's own parameter names are the cramped
/// `grblcol` / `specpwr` / `mtlness` forms rather than spelled-out words.
///
/// @param token The token32. Zero is invalid (the client asserts on it).
/// @return The decoded name, or an empty string for an invalid token.
inline std::string tokenDecode32(uint32_t token) {
    if (token == 0) return {};
    uint32_t v = token - 0x30000000u;   // wraps, exactly as the client's int does
    std::string out;
    while (v != 0 && out.size() < 7) {
        out.push_back(kTokenBase23Alphabet[v % 23u]);
        v /= 23u;
    }
    return out;
}

/// @brief Inverse of ::tokenDecode32, for looking a name up as an id.
///
/// The encoding is reversible by construction, so this is exact rather than a
/// search. Useful for asking "which token is `grblcol`" without keeping a
/// hand-written table in sync.
///
/// @return The token32, or 0 if @p name contains a character outside the
///         alphabet or is longer than 7 characters.
inline uint32_t tokenEncode32(const std::string& name) {
    if (name.empty() || name.size() > 7) return 0;
    uint32_t v = 0;
    for (size_t i = name.size(); i-- > 0;) {
        const char* p = nullptr;
        for (const char* a = kTokenBase23Alphabet; *a; ++a)
            if (*a == name[i]) { p = a; break; }
        if (!p) return 0;
        v = v * 23u + (uint32_t)(p - kTokenBase23Alphabet);
    }
    return v + 0x30000000u;
}

} // namespace gw2bgfx
