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

#include <set>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2_atex.hpp"
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

// Header-only peek at an MFT row's ATEX, for the resolution-pair check below.
static bool peekAtex(Gw2Dat& dat, size_t row, int& w, int& h, std::string& fmt) {
    if (row >= dat.mft_data_list.size()) return false;
    try {
        std::vector<uint8_t> b = decomp(dat, (uint32_t)row);
        if (b.size() >= 4 && b[0] == 'C') b[0] = 'A';      // CTEX -> ATEX alias
        castlemist::atex::Texture t = castlemist::atex::parse(b.data(), b.size());
        w = t.width; h = t.height; fmt = t.fmt_name;
        return w > 0 && h > 0;
    } catch (const std::exception&) { return false; }
}

int main(int argc, char** argv) {
    const std::string datPath = arg(argc, argv, "--dat");
    const std::string tplPath = arg(argc, argv, "--template", "dumps/packfile/gw2_packfile.json");
    uint32_t index = (uint32_t)std::stoul(arg(argc, argv, "--index", "291977"));
    // --fileid takes the number the archive browser and the MODL references use;
    // --index takes a raw MFT row. They differ by the baseId indirection, which
    // is the same lookup gw2bgfx_view does before loading a rig.
    const uint32_t fileId = (uint32_t)std::stoul(arg(argc, argv, "--fileid", "0"));
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
    // Ask for the skinned vertex feed rather than the static one.
    bool skinned = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--skinned") skinned = true;

    if (datPath.empty()) {
        std::fprintf(stderr, "usage: gw2bgfx_probe --dat <Gw2.dat> [--template <json>] "
                             "[--index <mftRow> | --fileid <id>] [--quality 0..4] [--skinned]\n");
        return 2;
    }

    nlohmann::json tpl;
    { std::ifstream f(tplPath, std::ios::binary);
      if (!f) { std::fprintf(stderr, "cannot open template %s\n", tplPath.c_str()); return 2; }
      f >> tpl; }

    Gw2Dat dat;
    load_dat_file(dat, datPath);

    if (fileId) {
        uint32_t base = get_by_base_id(dat, fileId);
        if (!base || base - 1 >= dat.mft_data_list.size()) {
            std::fprintf(stderr, "fileId %u does not resolve to an MFT row\n", fileId);
            return 2;
        }
        index = base - 1;
        std::printf("fileId %u -> baseId %u -> MFT row %u\n", fileId, base, index);
    }

    std::vector<uint8_t> modlBytes = decomp(dat, index);
    mdl::Extractor ex(modlBytes, tpl);
    mdl::Model model = ex.extract();                 // materials, for tokens and texture lists
    std::vector<mdl::GeosetRaw> geosets = ex.extractGeosetsRaw();

    // The rig, resolved the same way gw2bgfx_view does: inline when the MODL
    // carries one, otherwise the SKEL chunk's fileReference names another
    // model's. Needed here so the table can say whether each geoset's bone
    // bindings actually reach a bone -- an unresolved binding is a piece of the
    // model that will not follow the animation.
    mdl::Model extRig;
    const mdl::Skeleton* skel = &model.skeleton;
    if (model.skeleton.bones.empty() && model.skeleton.externalRef != 0) {
        uint32_t rigBase = get_by_base_id(dat, model.skeleton.externalRef);
        if (rigBase && rigBase - 1 < dat.mft_data_list.size()) {
            try {
                std::vector<uint8_t> rigBytes = decomp(dat, rigBase - 1);
                extRig = mdl::Extractor(rigBytes, tpl).extract();
                if (!extRig.skeleton.bones.empty()) skel = &extRig.skeleton;
            } catch (const std::exception&) { /* leave unresolved; table shows 0/n */ }
        }
    }
    std::unordered_map<uint64_t, int> tokMap;
    for (size_t i = 0; i < skel->bones.size(); ++i)
        tokMap[mdl::tokenizeBoneName(skel->bones[i].name)] = (int)i;

    std::printf("model %u: %zu geosets, %zu materials, rig %zu bones%s\n\n",
                index, geosets.size(), model.materials.size(), skel->bones.size(),
                skel == &extRig.skeleton ? " (external)" : "");

    // ---------------------------------------------------------------------
    // Geometry: the stride identity
    // ---------------------------------------------------------------------
    std::printf("== geometry ==\n");
    std::printf("%-4s %-10s %7s %7s %7s %7s %7s %8s %6s  %s\n",
                "#", "fvf", "verts", "file", "ddi", "bgfx", "idx", "binds", "skin", "verdict");
    int strideBad = 0;
    for (size_t i = 0; i < geosets.size(); ++i) {
        const mdl::GeosetRaw& g = geosets[i];
        bgfx::VertexLayout layout = grFvfBuildVertexLayout(g.fvf);
        const uint32_t ddi = grFvfDdiStride(g.fvf);
        const uint32_t gpu = layout.getStride();
        const uint32_t file = g.stride();
        const bool ok = (ddi == gpu) && (file == ddi);
        if (!ok) ++strideBad;
        // Bone bindings vs. the per-vertex skin feed. A geoset can carry
        // bindings WITHOUT weights+indices: that is GW2's rigid attach, and
        // it still has to follow the rig.
        const bool skinFeed = (g.fvf & GR_FVF_WEIGHTS) && (g.fvf & GR_FVF_GROUP);
        size_t resolved = 0;
        for (uint64_t tok : g.boneBindings)
            if (tokMap.count(tok)) ++resolved;
        char bind[24];
        std::snprintf(bind, sizeof(bind), "%zu/%zu", resolved, g.boneBindings.size());
        std::printf("%-4zu 0x%08X %7u %7u %7u %7u %7zu %8s %6s  %s\n",
                    i, g.fvf, g.vertexCount, file, ddi, gpu, g.indices.size(), bind,
                    skinFeed ? "vtx" : (g.boneBindings.empty() ? "-" : "rigid"),
                    ok ? "upload verbatim" : "MISMATCH");
    }
    std::printf("\n%d of %zu geosets need conversion before upload.\n\n",
                strideBad, geosets.size());

    // ---------------------------------------------------------------------
    // Textures: what resolution actually reaches the GPU
    // ---------------------------------------------------------------------
    // The renderer uploads atex mip 0 and nothing else, so mip 0's size IS the
    // sampled resolution. A texture whose stored mip 0 is far below its declared
    // header size, or that fails to parse and falls back to the 1x1 stand-in, is
    // the difference between a crisp surface and a smeared one.
    std::printf("== textures ==\n");
    std::printf("%-10s %-6s %-10s %11s %5s %s\n",
                "fileId", "fourCC", "format", "header", "mips", "mip0 (what is sampled)");
    {
        std::set<uint32_t> seen;
        for (const mdl::Material& mat : model.materials)
            for (const auto& tex : mat.textures) {
                if (!tex.fileId || !seen.insert(tex.fileId).second) continue;
                uint32_t tb = get_by_base_id(dat, tex.fileId);
                if (!tb || tb - 1 >= dat.mft_data_list.size()) {
                    std::printf("%-10u %-6s %-10s %11s %5s %s\n",
                                tex.fileId, "-", "-", "-", "-", "NOT IN MFT -> 1x1 white stand-in");
                    continue;
                }
                try {
                    std::vector<uint8_t> tb2 = decomp(dat, tb - 1);
                    castlemist::atex::Texture t = castlemist::atex::parse(tb2.data(), tb2.size());
                    char cc[5] = {t.fourcc[0], t.fourcc[1], t.fourcc[2], t.fourcc[3], 0};
                    char hdr[16], m0[64];
                    std::snprintf(hdr, sizeof(hdr), "%dx%d", t.width, t.height);
                    if (t.mips.empty()) {
                        std::snprintf(m0, sizeof(m0), "NO MIPS -> decode throws -> 1x1 white");
                    } else {
                        const auto& m = t.mips.front();
                        std::snprintf(m0, sizeof(m0), "%dx%d%s", m.width, m.height,
                                      (m.width == t.width && m.height == t.height)
                                          ? "" : "  <-- SMALLER THAN HEADER");
                    }
                    // GW2 ships most textures as a PAIR of MFT rows: a reduced
                    // member at baseId B-1 and the full one at B, same format,
                    // exactly double the dimensions. Which member a material's
                    // fileId lands on is not consistent, so a renderer that
                    // takes the row verbatim samples the half-size copy on some
                    // materials and the full one on others -- and half size
                    // magnified over a preview viewport reads as "blurry".
                    char sib[64] = "";
                    { int w1, h1; std::string f1;
                      size_t row = tb - 1;
                      if (peekAtex(dat, row + 1, w1, h1, f1) && f1 == t.fmt_name
                          && w1 == 2 * t.width && h1 == 2 * t.height)
                          std::snprintf(sib, sizeof(sib), "  REDUCED -- full %dx%d at row %zu",
                                        w1, h1, row + 1);
                      else if (row >= 1 && peekAtex(dat, row - 1, w1, h1, f1) && f1 == t.fmt_name
                               && t.width == 2 * w1 && t.height == 2 * h1)
                          std::snprintf(sib, sizeof(sib), "  full (reduced sibling below)");
                    }
                    std::printf("%-10u %-6s %-10s %11s %5zu %s%s\n",
                                tex.fileId, cc, t.fmt_name.c_str(), hdr, t.mips.size(), m0, sib);
                } catch (const std::exception& e) {
                    std::printf("%-10u %-6s %-10s %11s %5s parse failed: %s -> 1x1 white\n",
                                tex.fileId, "?", "?", "?", "?", e.what());
                }
            }
    }
    std::printf("\n");

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

        // The mesh flags decide the vertex feed. Default to the plain variant;
        // `--skinned` sets the weights+indices bits, which is what a rigged
        // geoset carries and the only route to the vertex shader that declares
        // `grbones` (variant 1 -- see GrVsVariant).
        const uint32_t variant =
            vsVariantFromMeshFlags(/*meshFlags=*/skinned ? (GR_FVF_WEIGHTS | GR_FVF_GROUP) : 0u,
                                   /*surfaceFlags=*/0,
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

            // Every vertex feed this effect offers, and which of them actually
            // read the engine's bone palette. This is what says whether the
            // variant the draw loop asks for is the GPU-skinned one -- the host
            // has to feed `grbones` to exactly those and no others.
            std::printf("               variants:");
            for (const auto& vv : sel.effect->vertexShaderVariants) {
                bool bones = false;
                if (vv.vertexShaderIndex < pkg.shaders.size())
                    for (const auto& u : parseBgfxBlobUniforms(pkg.shaders[vv.vertexShaderIndex].dx11Shader.data))
                        if (u.name == "grbones") { bones = true; break; }
                std::printf(" v%u->vs%u%s", vv.variant, vv.vertexShaderIndex, bones ? "[grbones]" : "");
            }
            std::printf("\n");
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
