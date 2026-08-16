/// @file
/// @brief Runs the whole faithful path over one real model and reports what
///        the client would do, without rendering anything.
///
/// This exists because "does it look right" is a bad test for a 1:1 renderer:
/// it conflates the geometry path, the selection path and the shading path. The
/// probe checks the two that have definite right answers.
///
///  - **Geometry.** Does `DDI_STRIDE(fvf)` equal both the file's own bytes-per-
///    vertex and the bgfx layout stride? If yes, the vertex buffer uploads
///    verbatim and there is no repacking left to get wrong.
///  - **Selection.** Does the AMAT contain an effect at the render-mode token
///    we are asking for, so the client's own rule answers -- or is the default
///    fallback answering instead? When it falls back, the probe lists every
///    token the pass does offer, which is what showed the selection token to be
///    a render mode rather than a material id.
///
/// @code
/// gw2bgfx_probe --dat "<...>/Gw2.dat" --template dumps/packfile/gw2_packfile.json --index 291977
/// @endcode

#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"

#include "amat_load.h"
#include "bgfx_draw.h"
#include "gr_fvf.h"
#include "gr_token.h"

using namespace gw2bgfx;
namespace mdl = castlemist::model;

static std::string arg(int argc, char** argv, const char* key, const char* def = "") {
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}

static std::vector<uint8_t> decomp(Gw2Dat& dat, uint32_t idx) {
    const MftData& e = dat.mft_data_list[idx];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0) return s;
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
    return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
}

int main(int argc, char** argv) {
    const std::string datPath = arg(argc, argv, "--dat");
    const std::string tplPath = arg(argc, argv, "--template", "dumps/packfile/gw2_packfile.json");
    const uint32_t index = (uint32_t)std::stoul(arg(argc, argv, "--index", "291977"));
    // The engine's quality cap. Ultra is the top tier; lowering it here shows
    // which technique the client would pick on lower settings.
    const int maxQuality = std::stoi(arg(argc, argv, "--quality", "4"));
    // The effect token to select with.
    //
    // NOT the MODL material's token. The client reads this from the runtime
    // McMaterial (`*(mesh->material + 64)`) and it identifies the RENDER MODE
    // the calling pass wants -- opaque, faded, decal -- which is why one AMAT
    // offers a fixed menu of the same tokens across every material, and why
    // three of them are the hardcoded remap-chain sources.
    //
    // 0x914C6A8A883B1EE is the opaque one: the paper-doll widget writes exactly
    // that token when its fade is 1.0, and 0x51C59061370654 (the 2nd link of
    // the remap chain) when it is faded. Default to opaque.
    const uint64_t effectToken = std::stoull(arg(argc, argv, "--token", "0x914C6A8A883B1EE"), nullptr, 0);

    if (datPath.empty()) {
        std::fprintf(stderr, "usage: gw2bgfx_probe --dat <Gw2.dat> [--template <json>] "
                             "[--index <baseId>] [--quality 0..4]\n");
        return 2;
    }

    nlohmann::json tpl;
    { std::ifstream f(tplPath, std::ios::binary);
      if (!f) { std::fprintf(stderr, "cannot open template %s\n", tplPath.c_str()); return 2; }
      f >> tpl; }

    Gw2Dat dat;
    load_dat_file(dat, datPath);

    std::vector<uint8_t> modlBytes = decomp(dat, index);
    mdl::Extractor ex(modlBytes, tpl);
    mdl::Model model = ex.extract();                 // materials, for tokens and texture lists
    std::vector<mdl::GeosetRaw> geosets = ex.extractGeosetsRaw();

    std::printf("model %u: %zu geosets, %zu materials\n\n",
                index, geosets.size(), model.materials.size());

    // ---------------------------------------------------------------------
    // Geometry: the stride identity
    // ---------------------------------------------------------------------
    std::printf("== geometry ==\n");
    std::printf("%-4s %-10s %7s %7s %7s %7s %7s  %s\n",
                "#", "fvf", "verts", "file", "ddi", "bgfx", "idx", "verdict");
    int strideBad = 0;
    for (size_t i = 0; i < geosets.size(); ++i) {
        const mdl::GeosetRaw& g = geosets[i];
        bgfx::VertexLayout layout = grFvfBuildVertexLayout(g.fvf);
        const uint32_t ddi = grFvfDdiStride(g.fvf);
        const uint32_t gpu = layout.getStride();
        const uint32_t file = g.stride();
        const bool ok = (ddi == gpu) && (file == ddi);
        if (!ok) ++strideBad;
        std::printf("%-4zu 0x%08X %7u %7u %7u %7u %7zu  %s\n",
                    i, g.fvf, g.vertexCount, file, ddi, gpu, g.indices.size(),
                    ok ? "upload verbatim" : "MISMATCH");
    }
    std::printf("\n%d of %zu geosets need conversion before upload.\n\n",
                strideBad, geosets.size());

    // ---------------------------------------------------------------------
    // Selection: run the engine's own rule per material
    // ---------------------------------------------------------------------
    std::printf("== materials ==\n");
    for (const mdl::Material& mat : model.materials) {
        std::printf("material %u  token 0x%llX  flags 0x%X  sortOrder %u  textures %zu\n",
                    mat.index, (unsigned long long)mat.token, mat.materialFlags,
                    mat.sortOrder, mat.textures.size());

        if (!mat.materialFile) { std::printf("    no material file\n\n"); continue; }
        uint32_t fnBase = get_by_base_id(dat, mat.materialFile);
        if (!fnBase) { std::printf("    material file %u not in the MFT\n\n", mat.materialFile); continue; }

        // ArenaNet groups a material's assets in consecutive baseIds: the AMAT
        // shader package sits one before the material's first texture.
        std::vector<uint8_t> amatBytes;
        try { amatBytes = decomp(dat, fnBase - 1); }
        catch (const std::exception& e) { std::printf("    AMAT read failed: %s\n\n", e.what()); continue; }

        mdl::AmatTree raw;
        try { raw = mdl::Extractor(amatBytes, tpl).extractAmatTree(); }
        catch (const std::exception& e) { std::printf("    AMAT parse failed: %s\n\n", e.what()); continue; }

        AmatPackage pkg = convertAmat(raw);
        if (!pkg.ok()) { std::printf("    AMAT unusable: %s\n\n", pkg.error.c_str()); continue; }

        int quality = 0;
        int tech = amatSelectTechnique(pkg, maxQuality, &quality);
        static const char* kQualityName[] = {"invalid", "low", "medium", "high", "ultra"};
        std::printf("    AMAT baseId %u: %zu shaders, %zu techniques, %zu sampler states\n",
                    fnBase - 1, pkg.shaders.size(), pkg.techniques.size(), pkg.samplerStates.size());
        std::printf("    technique %d (%s) of %zu\n", tech,
                    (quality >= 0 && quality <= 4) ? kQualityName[quality] : "?",
                    pkg.techniques.size());
        if (tech < 0) { std::printf("\n"); continue; }

        // The mesh flags decide the vertex feed. This model is unskinned and
        // uninstanced, so the static variant, but derive it rather than assume.
        const uint32_t variant = vsVariantFromMeshFlags(/*meshFlags=*/0, /*surfaceFlags=*/0,
                                                        /*instanced=*/false);

        for (size_t p = 0; p < pkg.techniques[tech].passes.size(); ++p) {
            AmatSelection sel = amatSelectEffect(pkg, tech, (uint32_t)p, effectToken, variant);
            // When the material's own token did not answer, list what the pass
            // does offer. A pass whose tokens are all unrelated to the
            // material's means the default is genuinely the right answer; a
            // pass that contains the material's token but was not matched
            // means we are reading the wrong field off the MODL.
            if (!sel.ok || !sel.matchedMaterialToken) {
                std::printf("      pass %zu offers %zu effect tokens:", p,
                            pkg.techniques[tech].passes[p].effects.size());
                for (const auto& e : pkg.techniques[tech].passes[p].effects)
                    std::printf(" 0x%llX", (unsigned long long)e.token);
                std::printf("\n");
            }
            if (!sel.ok) {
                std::printf("      pass %zu: no effect (material does not draw in this pass)\n", p);
                continue;
            }

            GrSurfaceState surf;
            surf.materialToken = effectToken;
            GrDrawState st = grComposeDrawState(*sel.effect, (uint32_t)p, surf);

            const auto& vsBlob = pkg.shaders[sel.vertexShaderIndex].dx11Shader;
            const auto& psBlob = pkg.shaders[sel.pixelShaderIndex].dx11Shader;
            BgfxBlobInfo vi = inspectBgfxBlob(vsBlob.data);
            BgfxBlobInfo pi = inspectBgfxBlob(psBlob.data);

            std::printf("      pass %zu: effect token 0x%-14llX %s  vs %u  ps %u  variant %u\n",
                        p, (unsigned long long)sel.effect->token,
                        sel.matchedMaterialToken ? "(material token)" : "(FALLBACK)      ",
                        sel.vertexShaderIndex, sel.pixelShaderIndex, sel.variant);
            std::printf("               passFlags 0x%-6X renderState 0x%016llX -> state 0x%016llX%s\n",
                        sel.effect->shaderPassFlags,
                        (unsigned long long)sel.effect->renderState,
                        (unsigned long long)st.state,
                        st.depthBias != 0.0f ? "  [depth bias]" : "");
            std::printf("               vs blob %zu B %cSH v%u, %u uniforms, %zu constants, %zu samplers\n",
                        vsBlob.data.size(), vi.valid ? vi.kind : '?', vi.version,
                        vi.uniformCount, vsBlob.constants.size(), vsBlob.samplers.size());
            std::printf("               ps blob %zu B %cSH v%u, %u uniforms, %zu constants, %zu samplers\n",
                        psBlob.data.size(), pi.valid ? pi.kind : '?', pi.version,
                        pi.uniformCount, psBlob.constants.size(), psBlob.samplers.size());

            // The engine's own count assert (BgfxShader.cpp:336). If this does
            // not hold, the constants[]/samplers[] cursors would drift against
            // the shader's uniform list and every material parameter after the
            // first mismatch would land in the wrong slot.
            auto checkPairing = [](const char* what, const AmatShaderBinary& b, uint32_t uniforms) {
                size_t skipped = 0;
                for (const auto& s : b.samplers)
                    if (s.textureIndex == 0xFFFFFFFFu) ++skipped;
                const size_t expect = b.constants.size() + b.samplers.size() - skipped;
                std::printf("               %s pairing: %zu constants + %zu samplers - %zu(-1) = %zu vs %u uniforms  %s\n",
                            what, b.constants.size(), b.samplers.size(), skipped, expect, uniforms,
                            expect == uniforms ? "OK" : "MISMATCH");
            };
            if (vi.valid) checkPairing("vs", vsBlob, vi.uniformCount);
            if (pi.valid) checkPairing("ps", psBlob, pi.uniformCount);

            // The uniform names the host has to supply values for. Walking the
            // table here also proves the walk itself: if the parse were off by
            // a byte the names would come out as mojibake immediately.
            static const char* kTypeName[] = {"smp", "end", "vec4", "mat3", "mat4"};
            for (const char* which : {"vs", "ps"}) {
                const AmatShaderBinary& b = (which[0] == 'v') ? vsBlob : psBlob;
                auto us = parseBgfxBlobUniforms(b.data);
                std::printf("               %s uniforms (%zu):", which, us.size());
                for (const auto& u : us)
                    std::printf(" %s:%s%s@%u", u.name.c_str(),
                                u.type() < 5 ? kTypeName[u.type()] : "?",
                                u.num > 1 ? "[]" : "", u.regIndex);
                std::printf("\n");
            }

            // Which texture goes to which register, and which are engine globals.
            for (const auto& s : psBlob.samplers) {
                const bool global = s.textureIndex >= mat.textures.size();
                std::printf("               s%-2u <- %s", s.textureSlot,
                            global ? "ENGINE GLOBAL" : "material tex ");
                if (!global) std::printf("%u (fileId %u)", s.textureIndex,
                                         mat.textures[s.textureIndex].fileId);
                else std::printf("(textureIndex %d)", (int)s.textureIndex);
                uint32_t state = s.stateIndex < pkg.samplerStates.size()
                                     ? pkg.samplerStates[s.stateIndex] : 0;
                std::printf("  samplerState 0x%X\n", state);
            }
        }

        // Per-material constants, decoded to their real names.
        if (!mat.constants.empty()) {
            std::printf("    constants:");
            for (const auto& c : mat.constants)
                std::printf(" %s=(%.3g,%.3g,%.3g,%.3g)",
                            tokenDecode32(c.name).c_str(), c.value[0], c.value[1], c.value[2], c.value[3]);
            std::printf("\n");
        }
        std::printf("\n");
    }
    return strideBad == 0 ? 0 : 1;
}
