/// @file
/// @brief Bridge from castlemist's packfile parser to the renderer's AMAT types.
///
/// `castlemist::model::AmatTree` is what came out of the file;
/// `gw2bgfx::AmatPackage` is what the ported selection logic consumes. They
/// carry the same fields, and this is the one place that says so -- keeping
/// `gw2bgfx_core` free of nlohmann/json and of the whole native layer, so the
/// selection ports stay unit-testable on their own.

#pragma once

#include "amat_effect.h"

#include "castlemist/native/gw2model.hpp"

namespace gw2bgfx {

inline AmatShaderBinary convertBinary(const castlemist::model::AmatShaderBinaryRaw& in) {
    AmatShaderBinary out;
    out.data = in.data;
    out.constants = in.constants;
    out.samplers.reserve(in.samplers.size());
    for (const auto& s : in.samplers)
        out.samplers.push_back(AmatSamplerConstant{s.token, s.stateIndex, s.textureIndex, s.textureSlot});
    return out;
}

inline AmatPackage convertAmat(const castlemist::model::AmatTree& in) {
    AmatPackage out;
    out.error = in.error;
    out.samplerStates = in.samplerStates;

    out.shaders.reserve(in.shaders.size());
    for (const auto& s : in.shaders) {
        AmatShader sh;
        sh.isPixelShader = s.isPixelShader;
        sh.dx11Shader = convertBinary(s.dx11Shader);
        sh.osxShader = convertBinary(s.osxShader);
        out.shaders.push_back(std::move(sh));
    }

    out.techniques.reserve(in.techniques.size());
    for (const auto& t : in.techniques) {
        AmatTechnique tech;
        tech.quality = t.quality;
        tech.passes.reserve(t.passes.size());
        for (const auto& p : t.passes) {
            AmatPass pass;
            pass.effects.reserve(p.effects.size());
            for (const auto& e : p.effects) {
                AmatEffect eff;
                eff.token = e.token;
                eff.renderState = e.renderState;
                eff.shaderPassFlags = e.shaderPassFlags;
                eff.pixelShaderIndex = e.pixelShaderIndex;
                eff.vertexShaderVariants.reserve(e.vertexShaderVariants.size());
                for (const auto& v : e.vertexShaderVariants)
                    eff.vertexShaderVariants.push_back(AmatVertexShaderVariant{v.variant, v.vertexShaderIndex});
                pass.effects.push_back(std::move(eff));
            }
            tech.passes.push_back(std::move(pass));
        }
        out.techniques.push_back(std::move(tech));
    }
    return out;
}

/// @brief The bgfx shader blob header, as far as the renderer cares.
///
/// Parsed only to *report* on a blob -- the bytes go to `bgfx::createShader`
/// whole, because bgfx's own `ShaderD3D11::create` is the parser that matters.
struct BgfxBlobInfo {
    bool valid = false;
    char kind = 0;        ///< 'V', 'F' or 'C'
    uint8_t version = 0;  ///< must be 11 for this client
    uint32_t uniformCount = 0;
    uint32_t codeSize = 0;
};

inline BgfxBlobInfo inspectBgfxBlob(const std::vector<uint8_t>& blob) {
    BgfxBlobInfo i;
    if (blob.size() < 12) return i;
    if (blob[1] != 'S' || blob[2] != 'H') return i;
    if (blob[0] != 'V' && blob[0] != 'F' && blob[0] != 'C') return i;
    i.kind = (char)blob[0];
    i.version = blob[3];
    // magic(4) hashIn(4) hashOut(4, version >= 6) then the uniform count.
    size_t p = 4 + 4 + (i.version >= 6 ? 4u : 0u);
    if (p + 2 > blob.size()) return i;
    i.uniformCount = (uint32_t)(blob[p] | (blob[p + 1] << 8));
    i.valid = true;
    return i;
}

/// @brief One entry of a blob's uniform table.
struct BgfxBlobUniform {
    std::string name;
    uint8_t rawType = 0;   ///< As stored, including the fragment/sampler bits.
    uint8_t num = 0;       ///< Array length.
    uint16_t regIndex = 0; ///< A BYTE offset into the shader's constant buffer.
    uint16_t regCount = 0;

    /// @brief `UniformType`: 0 Sampler, 1 End, 2 Vec4, 3 Mat3, 4 Mat4.
    uint8_t type() const { return (uint8_t)(rawType & ~0x30u); }
    bool isSampler() const { return (rawType & 0x20u) != 0; }
    bool isFragment() const { return (rawType & 0x10u) != 0; }
};

/// @brief Enumerate a blob's uniform table.
///
/// We hand the blob itself to `bgfx::createShader` untouched -- bgfx's own
/// parser is the one that matters. This walk exists only so the host knows
/// *which* uniforms to create handles for and feed, since bgfx offers no way to
/// enumerate them back off a shader handle at this revision.
///
/// Layout, from `BgfxShaderD3D11_Create` and confirmed against bgfx's
/// `ShaderD3D11::create`:
///
/// @verbatim
///   magic 'VSH'|'FSH'|'CSH' + version   4
///   hashIn                              4
///   hashOut                             4   (version >= 6)
///   count                               2
///   per uniform:
///     nameLen                           1
///     name                        nameLen
///     type                              1
///     num                               1
///     regIndex                          2
///     regCount                          2
///     texInfo                           2   (version >= 8)
///     texFormat                         2   (version >= 10)
/// @endverbatim
inline std::vector<BgfxBlobUniform> parseBgfxBlobUniforms(const std::vector<uint8_t>& blob) {
    std::vector<BgfxBlobUniform> out;
    BgfxBlobInfo info = inspectBgfxBlob(blob);
    if (!info.valid) return out;

    auto u16at = [&](size_t p) -> uint16_t {
        return (uint16_t)(blob[p] | (blob[p + 1] << 8));
    };

    size_t p = 4 + 4 + (info.version >= 6 ? 4u : 0u);
    const uint32_t count = u16at(p);
    p += 2;

    const size_t perTail = 1u + 1u + 2u + 2u
                         + (info.version >= 8 ? 2u : 0u)
                         + (info.version >= 10 ? 2u : 0u);

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 1 > blob.size()) break;
        const uint8_t nameLen = blob[p++];
        if (p + nameLen + perTail > blob.size()) break;

        BgfxBlobUniform u;
        u.name.assign((const char*)&blob[p], nameLen);
        p += nameLen;
        u.rawType = blob[p++];
        u.num = blob[p++];
        u.regIndex = u16at(p); p += 2;
        u.regCount = u16at(p); p += 2;
        if (info.version >= 8)  p += 2;   // texInfo
        if (info.version >= 10) p += 2;   // texFormat
        out.push_back(std::move(u));
    }
    return out;
}

} // namespace gw2bgfx
