/// @file
/// @brief The 1:1 renderer: GW2's own shaders, geometry and state, through the
///        bgfx the client itself links.
///
/// Nothing here translates render data. The AMAT shader blobs go to
/// `bgfx::createShader` as stored, the effect's `renderState` goes into the
/// state word the client would have built, the vertex buffers upload without
/// repacking, and the vertex layout is what `GrFvf_BuildVertexLayout` produces.
/// Where a value has to be supplied from outside the archive -- the lighting
/// rig, the camera -- it is taken from measured client values and the source is
/// named at the point of use.
///
/// @code
/// gw2bgfx_viewer --dat "<...>/Gw2.dat" --index 291977
/// @endcode
///
/// LMB drag orbits, wheel zooms, Esc quits.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

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

// ---------------------------------------------------------------------------
// Engine-global uniform values
//
// These are the values the engine supplies that do not come out of the archive.
// Every one is either measured from a real frame or read out of the client's
// .rdata; none is dialled in by eye. Sources are named per entry -- see
// docs/research/gw2-engine-uniform-values.md and gw2-preview-render.md.
// ---------------------------------------------------------------------------
namespace
{

    struct Vec4
    {
        float v[4];
    };

    /// The paper-doll studio rig, evaluated.
    ///
    /// The client's Equipment Preview does not light a character from the map. It
    /// builds a private scene and an eight-light rig baked into .rdata at
    /// 0x141B10520, then feeds it through `ShLight::AddLight`. The values below are
    /// that evaluation, and they were checked against a RenderDoc capture of the
    /// live client (worst error over 28 components: 2e-05, i.e. the dump's own
    /// rounding). Using them means a lone model here is lit by the same numbers the
    /// client sends its own preview window.
    ///
    /// The first enabled light becomes the sun rather than accumulating into the SH
    /// bands -- that asymmetry is in `ShLight::AddLight` itself and is why shSun is
    /// not simply the L1 direction.
    const std::map<std::string, Vec4> kEngineUniforms = {
        // --- the rig (captured values) ---
        {"shRed", {{0.19360f, 0.08209f, 0.13942f, 0.66239f}}},
        {"shGreen", {{0.22712f, 0.09183f, 0.18630f, 0.65143f}}},
        {"shBlue", {{0.29937f, 0.11538f, 0.25859f, 0.69094f}}},
        {"shSun", {{-0.46890f, -0.60577f, -0.64279f, 0.0f}}},
        {"shSunColor", {{0.95120f, 0.99645f, 1.05000f, 1.05f}}},
        // .w = -1 repeats the sun direction for the specular term.
        {"shSunData", {{-0.46890f, -0.60577f, -0.64279f, -1.0f}}},

        // --- backlight globals, which the preview overrides with constants ---
        {"BacklightColor", {{1.5f, 1.5f, 1.5f, 1.5f}}}, // xmmword_141AE3640
        {"BacklightDirection", {{0.66341f, 0.38302f, -0.64279f, 0.0f}}},

        // --- shadowing, explicitly disabled ---
        // The lit shaders do `shadow = saturate(v.w * WorldToShadowD.x +
        // WorldToShadowD.y)` and blend toward the sampled map. Zero here plus a
        // white gSs15 collapses the whole chain to 1.0 = fully lit, which is the
        // right answer with no shadow-caster pass. Leaving it unset is not the same
        // thing: it would multiply by whatever the uniform happened to hold.
        {"WorldToShadowA", {{0, 0, 0, 0}}},
        {"WorldToShadowB", {{0, 0, 0, 0}}},
        {"WorldToShadowC", {{0, 0, 0, 0}}},
        {"WorldToShadowD", {{0, 0, 0, 0}}},

        // --- no local lights offline: lit purely by the global rig ---
        {"LightCount", {{0, 0, 0, 0}}},
        {"LightPointAndSpotData", {{0, 0, 0, 0}}},

        // --- range compression. A VECTOR; broadcasting one value wrecks the
        // specular exponent, since .w scales it (pow(NdotH, gloss * .w)).
        // Measured identical in every material cbuffer of a real frame. ---
        {"LightBuffer", {{0.25f, 4.0f, 1.0f / 128.0f, 128.0f}}},
        {"TexelOffset", {{0, 0, 0, 0}}}, // 0 on D3D11; the D3D9-era half-pixel fixup

        // --- alpha test. 0.25 measured; the cutout shaders spell it saturate(2a)<0.5.
        // Zero here disables the cutout and draws foliage and grates as solid quads.
        {"AlphaRef", {{0.25f, 0.25f, 0.25f, 0.25f}}},

        // --- stipple fade. fxclr defaults to 0, which means "fully faded out", and
        // the stipple-discard shaders then discard EVERY pixel: the model renders
        // empty with correct geometry and bounds. ---
        {"fxclr", {{1, 1, 1, 1}}},
        {"StippleDensity", {{0, 0, 0, 0}}},
        {"StencilId", {{0, 0, 0, 0}}},

        // --- fog off ---
        {"FogColorNearMinusFar", {{0, 0, 0, 0}}},
        {"FogColorFar", {{0, 0, 0, 0}}},
        {"FogColorHeight", {{0, 0, 0, 0}}},
        {"FogDepthCue", {{0, 0, 0, 0}}},
        {"FogParam0", {{0, 0, 0, 0}}},

        // --- identity UV transforms; overridden per material where authored ---
        {"TexTransform0A", {{1, 0, 0, 0}}},
        {"TexTransform0B", {{0, 1, 0, 0}}},
        {"TexTransform1A", {{1, 0, 0, 0}}},
        {"TexTransform1B", {{0, 1, 0, 0}}},
        {"TexTransform2A", {{1, 0, 0, 0}}},
        {"TexTransform2B", {{0, 1, 0, 0}}},
        {"TexTransform3A", {{1, 0, 0, 0}}},
        {"TexTransform3B", {{0, 1, 0, 0}}},
    };

    /// @brief Routes bgfx's diagnostics to stderr.
    ///
    /// Without this bgfx logs through `bx::debugPrintf`, which on Windows goes to
    /// OutputDebugString -- invisible unless a debugger is attached. A failed
    /// `bgfx::init` then returns false with no explanation at all.
    struct BgfxCallback : public bgfx::CallbackI
    {
        virtual ~BgfxCallback() {}
        void fatal(const char *filePath, uint16_t line, bgfx::Fatal::Enum code, const char *str) override
        {
            std::fprintf(stderr, "[bgfx FATAL %d] %s:%u: %s\n", (int)code, filePath, line, str);
        }
        void traceVargs(const char *filePath, uint16_t line, const char *format, va_list argList) override
        {
            std::fprintf(stderr, "[bgfx] %s:%u: ", filePath, line);
            std::vfprintf(stderr, format, argList);
        }
        void profilerBegin(const char *, uint32_t, const char *, uint16_t) override {}
        void profilerBeginLiteral(const char *, uint32_t, const char *, uint16_t) override {}
        void profilerEnd() override {}
        uint32_t cacheReadSize(uint64_t) override { return 0; }
        bool cacheRead(uint64_t, void *, uint32_t) override { return false; }
        void cacheWrite(uint64_t, const void *, uint32_t) override {}
        void screenShot(const char *, uint32_t, uint32_t, uint32_t, const void *, uint32_t, bool) override {}
        void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
        void captureEnd() override {}
        void captureFrame(const void *, uint32_t) override {}
    };
    BgfxCallback g_bgfxCallback;

    std::string argOf(int argc, char **argv, const char *key, const char *def = "")
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (!std::strcmp(argv[i], key))
                return argv[i + 1];
        return def;
    }

    std::vector<uint8_t> decomp(Gw2Dat &dat, uint32_t idx)
    {
        const MftData &e = dat.mft_data_list[idx];
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> s = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
        if (e.compression_flag == 0)
            return s;
        uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
        return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
    }

    // --- uniform handle cache -------------------------------------------------
    // bgfx dedupes uniforms by name internally, but the host still needs a handle
    // per name to call setUniform, so keep one map for the whole run.
    std::map<std::string, bgfx::UniformHandle> g_uniforms;

    bgfx::UniformHandle uniformFor(const BgfxBlobUniform &u)
    {
        auto it = g_uniforms.find(u.name);
        if (it != g_uniforms.end())
            return it->second;

        bgfx::UniformType::Enum type = bgfx::UniformType::Vec4;
        if (u.isSampler())
            type = bgfx::UniformType::Sampler;
        else if (u.type() == 3)
            type = bgfx::UniformType::Mat3;
        else if (u.type() == 4)
            type = bgfx::UniformType::Mat4;

        bgfx::UniformHandle h = bgfx::createUniform(u.name.c_str(), type, std::max<uint8_t>(1, u.num));
        g_uniforms[u.name] = h;
        return h;
    }

    /// @brief One drawable submesh, fully resolved.
    struct Draw
    {
        bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        std::vector<BgfxBlobUniform> vsU, psU;
        /// Sampler slot -> texture. Parallel to the effect's sampler list.
        std::vector<std::pair<uint8_t, bgfx::TextureHandle>> textures;
        /// Sampler uniforms, matched to the slots above by textureSlot.
        std::vector<AmatSamplerConstant> samplers;
        /// MODL material constants, decoded to uniform names.
        std::map<std::string, Vec4> matConsts;
        uint64_t state = 0;
        uint32_t indexCount = 0;
    };

} // namespace

// ---------------------------------------------------------------------------
// Win32 shell
// ---------------------------------------------------------------------------
namespace
{
    /// Trackball orientation, accumulated as a matrix rather than as Euler
    /// angles.
    ///
    /// With yaw/pitch the camera's up vector has to be derived from the pitch,
    /// and past +-90 deg it inverts: a horizontal drag suddenly spins the other
    /// way, so a long drag reads as the model jumping around at random. Turning
    /// the OBJECT by a composed increment and leaving the camera still has
    /// neither a pole nor a gimbal, and it matches what castlemist's own
    /// renderer does.
    float g_rot[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// Camera distance as a multiple of the bounding radius.
    float g_distMul = 3.0f;
    bool g_rotDirty = true;
    POINT g_last{};
    bool g_dragging = false;
    bool g_quit = false;
    /// Live backbuffer size. WM_SIZE writes it, the frame loop reads it and
    /// re-resets bgfx; the aspect ratio is derived from it every frame rather
    /// than from the launch size, so the projection follows the window instead
    /// of stretching whatever was baked in at startup.
    int g_width = 1280, g_height = 800;
    bool g_sizeDirty = false;

    LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_SIZE:
        {
            // A minimise reports 0x0; resetting to that kills the swap chain.
            const int cw = LOWORD(l), ch = HIWORD(l);
            if (w != SIZE_MINIMIZED && cw > 0 && ch > 0 &&
                (cw != g_width || ch != g_height))
            {
                g_width = cw;
                g_height = ch;
                g_sizeDirty = true;
            }
            return 0;
        }
        case WM_DESTROY:
            g_quit = true;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE)
            {
                g_quit = true;
                PostQuitMessage(0);
            }
            return 0;
        case WM_LBUTTONDOWN:
            g_dragging = true;
            g_last = {LOWORD(l), HIWORD(l)};
            SetCapture(h);
            return 0;
        case WM_LBUTTONUP:
            g_dragging = false;
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE:
            if (g_dragging)
            {
                int x = LOWORD(l), y = HIWORD(l);
                // Compose the drag as a rotation applied AFTER the current
                // orientation. No clamped axis and no gimbal lock, so a drag
                // means the same thing whichever way the model already faces.
                float ry[16], rx[16], inc[16], out[16];
                bx::mtxRotateY(ry, (x - g_last.x) * 0.01f);
                bx::mtxRotateX(rx, (y - g_last.y) * 0.01f);
                bx::mtxMul(inc, ry, rx);
                bx::mtxMul(out, g_rot, inc);
                std::memcpy(g_rot, out, sizeof(out));
                g_rotDirty = true;
                g_last = {x, y};
            }
            return 0;
        case WM_MOUSEWHEEL:
            g_distMul *= (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0.9f : 1.1f);
            g_distMul = std::clamp(g_distMul, 0.2f, 20.0f);
            return 0;
        }
        return DefWindowProc(h, m, w, l);
    }
} // namespace

int main(int argc, char **argv)
{
    const std::string datPath = argOf(argc, argv, "--dat");
    const std::string tplPath = argOf(argc, argv, "--template", "dumps/packfile/gw2_packfile.json");
    const uint32_t index = (uint32_t)std::stoul(argOf(argc, argv, "--index", "291977"));
    const int maxQuality = std::stoi(argOf(argc, argv, "--quality", "4"));
    // Opaque render mode. See amat_effect.h: this is a render-mode token, not a
    // material id, and it is the one the paper-doll writes when not fading.
    const uint64_t effectToken =
        std::stoull(argOf(argc, argv, "--token", "0x914C6A8A883B1EE"), nullptr, 0);
    // Per-model orientation trim, degrees, applied in model space before the
    // Z-up -> Y-up conversion. The archive does not agree with itself about
    // which way up a geoset sits: props like 291977 come out upright, while a
    // character's armour pieces are stored in a bind-pose space whose root
    // rotation lives in the skeleton this tool does not read, so they arrive
    // inverted. Rather than guess a global rule that is wrong half the time,
    // this is the manual override -- `--rot 180,0,0` stands the armour up.
    float rotDeg[3] = {0, 0, 0};
    {
        const std::string r = argOf(argc, argv, "--rot", "0,0,0");
        std::sscanf(r.c_str(), "%f,%f,%f", &rotDeg[0], &rotDeg[1], &rotDeg[2]);
    }
    const int startW = std::stoi(argOf(argc, argv, "--width", "1280"));
    const int startH = std::stoi(argOf(argc, argv, "--height", "800"));
    g_width = startW;
    g_height = startH;

    if (datPath.empty())
    {
        std::fprintf(stderr,
                     "usage: gw2bgfx_viewer --dat <Gw2.dat> [--index <mftIndex>] "
                     "[--template <json>] [--quality 0..4] [--token <hex>]\n"
                     "                      [--rot <x,y,z degrees>] [--width N] [--height N]\n");
        return 2;
    }

    // --- window ------------------------------------------------------------
    // Wide API throughout. The tool is built with -DUNICODE, so the unsuffixed
    // Win32 macros already resolve to the W entry points; registering an ANSI
    // class and creating an ANSI window underneath them mixes the two halves,
    // and the window then comes up with its title read as UTF-16 (the title bar
    // shows CJK) or CreateWindow fails outright and returns NULL. A NULL nwh is
    // what bgfx calls headless, and it refuses a headless device with a
    // non-zero backbuffer -- which is the "bgfx::init failed" above.
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"gw2bgfx";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc))
    {
        std::fprintf(stderr, "RegisterClassW failed (%lu)\n", GetLastError());
        return 1;
    }
    RECT r{0, 0, startW, startH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"gw2bgfx", L"gw2bgfx -- GW2's own shaders", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
    // bgfx reports a NULL window handle only as "headless", several layers from
    // the cause, so fail here where the reason is still visible.
    if (!hwnd)
    {
        std::fprintf(stderr, "CreateWindowW failed (%lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    // --- bgfx --------------------------------------------------------------
    bgfx::Init init;
    // Direct3D 11 explicitly: it is the backend the client ships, so it is the
    // one whose state translation we are matching.
    init.type = bgfx::RendererType::Direct3D11;
    init.resolution.width = (uint32_t)g_width;
    init.resolution.height = (uint32_t)g_height;
    init.resolution.reset = BGFX_RESET_VSYNC;
    init.platformData.nwh = hwnd;
    init.callback = &g_bgfxCallback;
    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx::init failed\n");
        return 1;
    }
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20242bff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, uint16_t(g_width), uint16_t(g_height));

    // --- archive -----------------------------------------------------------
    nlohmann::json tpl;
    {
        std::ifstream f(tplPath, std::ios::binary);
        if (!f)
        {
            std::fprintf(stderr, "cannot open template %s\n", tplPath.c_str());
            return 2;
        }
        f >> tpl;
    }

    Gw2Dat dat;
    load_dat_file(dat, datPath);

    std::vector<uint8_t> modlBytes = decomp(dat, index);
    mdl::Extractor ex(modlBytes, tpl);
    mdl::Model model = ex.extract();
    std::vector<mdl::GeosetRaw> geosets = ex.extractGeosetsRaw();
    if (geosets.empty())
    {
        std::fprintf(stderr, "model %u has no geosets\n", index);
        return 1;
    }

    // Bounding sphere, for framing the camera.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto &g : geosets)
        for (int i = 0; i < 3; ++i)
        {
            lo[i] = std::min(lo[i], g.minB[i]);
            hi[i] = std::max(hi[i], g.maxB[i]);
        }
    const float centreModel[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f,
                                  (lo[2] + hi[2]) * 0.5f};
    float radius = 0.0f;
    for (int i = 0; i < 3; ++i)
        radius = std::max(radius, (hi[i] - lo[i]) * 0.5f);
    if (!(radius > 0.0f))
        radius = 1.0f;

    // --- model -> render space ---------------------------------------------
    // GW2 authors Z-up; the camera below is an ordinary Y-up orbit, which is
    // what every other D3D11 tool and every piece of bx's math assumes. Doing
    // the change of basis here, once, in the model's world matrix, is what
    // "rotate the model to match the axes" means: the geometry is handed to the
    // shader already in the renderer's space, and the camera never has to carry
    // a GW2-specific up vector around.
    //
    // Basis map (row-vector convention, matching bx): x -> x, y -> -z, z -> y.
    // A rotation, so lengths and therefore `radius` are unchanged; only the
    // centre has to be carried across.
    float axisFix[16];
    bx::mtxIdentity(axisFix);
    axisFix[0] = 1.0f;  axisFix[1] = 0.0f;  axisFix[2] = 0.0f;   // e_x -> ( 1, 0, 0)
    axisFix[4] = 0.0f;  axisFix[5] = 0.0f;  axisFix[6] = -1.0f;  // e_y -> ( 0, 0,-1)
    axisFix[8] = 0.0f;  axisFix[9] = 1.0f;  axisFix[10] = 0.0f;  // e_z -> ( 0, 1, 0)

    // The per-model trim runs first, in the model's own space.
    float trim[16];
    bx::mtxRotateXYZ(trim, bx::toRad(rotDeg[0]), bx::toRad(rotDeg[1]), bx::toRad(rotDeg[2]));

    // `base` is model -> render space. The trackball is folded in per frame,
    // rotating about the model's own centre rather than the origin: a model
    // whose bounds sit far off origin would otherwise swing around the scene
    // instead of turning in place.
    float base[16];
    bx::mtxMul(base, trim, axisFix);
    float world[16];
    std::memcpy(world, base, sizeof(world));

    // Measured through `base` only: the trackball then rotates ABOUT this
    // centre, so folding the rotation in first would make the pivot chase its
    // own result.
    const bx::Vec3 centreV =
        bx::mul(bx::Vec3(centreModel[0], centreModel[1], centreModel[2]), base);
    const float centre[3] = {centreV.x, centreV.y, centreV.z};
    std::fprintf(stderr, "[orient] rot=(%g,%g,%g) deg, centre model=(%g,%g,%g) -> render=(%g,%g,%g)"
                         " radius=%g\n",
                 rotDeg[0], rotDeg[1], rotDeg[2], centreModel[0], centreModel[1], centreModel[2],
                 centre[0], centre[1], centre[2], radius);

    // --- shared stand-ins for engine-global textures ------------------------
    // A sampler whose textureIndex is past the material's texture list is an
    // engine global we have no offline source for. White at gSs15 means
    // "unshadowed", which with WorldToShadowD = 0 is the correct answer for a
    // scene with no shadow-caster pass.
    auto solid2D = [](uint8_t v)
    {
        const bgfx::Memory *mem = bgfx::alloc(4);
        mem->data[0] = mem->data[1] = mem->data[2] = v;
        mem->data[3] = 255;
        return bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, mem);
    };
    const bgfx::TextureHandle texWhite = solid2D(255);
    // Neutral grey, not white: a white cube at the reflection slot turns every
    // metal into a mirror.
    const bgfx::Memory *cubeMem = bgfx::alloc(6 * 4);
    for (int f = 0; f < 6; ++f)
    {
        cubeMem->data[f * 4 + 0] = cubeMem->data[f * 4 + 1] = cubeMem->data[f * 4 + 2] = 64;
        cubeMem->data[f * 4 + 3] = 255;
    }
    const bgfx::TextureHandle texCube =
        bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::RGBA8, 0, cubeMem);

    // --- build the draws ----------------------------------------------------
    std::map<uint32_t, bgfx::TextureHandle> texByFileId;
    auto loadTexture = [&](uint32_t fileId) -> bgfx::TextureHandle
    {
        auto it = texByFileId.find(fileId);
        if (it != texByFileId.end())
            return it->second;
        bgfx::TextureHandle h = BGFX_INVALID_HANDLE;
        uint32_t base = get_by_base_id(dat, fileId);
        if (base && base - 1 < dat.mft_data_list.size())
        {
            try
            {
                // base - 1, exactly as the AMAT load below does. get_by_base_id
                // returns a one-past index; reading `base` lands on the NEXT
                // archive entry. That mistake does not announce itself: on
                // 291977 nine of the twenty textures landed on a 'PF' packfile
                // and threw "ATEX: bad magic" into the silent white fallback,
                // and the other eleven landed on a neighbouring ATEX, parsed
                // clean, and bound the wrong image -- a DXT1 where the DXT5
                // belonged. Hence flat white albedos and garbage normal maps.
                std::vector<uint8_t> bytes = decomp(dat, base - 1);
                castlemist::atex::Texture t = castlemist::atex::parse(bytes.data(), bytes.size());
                castlemist::atex::Image im = castlemist::atex::decode(t, 0);
                if (im.width > 0 && im.height > 0)
                {
                    const bgfx::Memory *mem = bgfx::copy(im.rgba.data(), (uint32_t)im.rgba.size());
                    h = bgfx::createTexture2D((uint16_t)im.width, (uint16_t)im.height, false, 1,
                                              bgfx::TextureFormat::RGBA8, 0, mem);
                }
            }
            catch (const std::exception &)
            { /* fall through to the stand-in */
            }
        }
        if (!bgfx::isValid(h))
            h = texWhite;
        texByFileId[fileId] = h;
        return h;
    };

    std::map<uint32_t, AmatPackage> amatByMaterial;
    std::vector<Draw> draws;

    for (const mdl::GeosetRaw &g : geosets)
    {
        if (g.vertexBytes.empty() || g.indices.empty())
            continue;

        bgfx::VertexLayout layout = grFvfBuildVertexLayout(g.fvf);
        if (!grFvfStrideMatches(g.fvf, layout))
        {
            std::fprintf(stderr, "geoset fvf 0x%08X needs conversion (ddi %u vs gpu %u), skipped\n",
                         g.fvf, grFvfDdiStride(g.fvf), layout.getStride());
            continue;
        }

        const mdl::Material *mat = nullptr;
        for (const auto &m : model.materials)
            if (m.index == g.materialIndex)
            {
                mat = &m;
                break;
            }
        if (!mat)
            continue;

        // AMAT, cached per material slot.
        auto ai = amatByMaterial.find(g.materialIndex);
        if (ai == amatByMaterial.end())
        {
            AmatPackage pkg;
            uint32_t fnBase = mat->materialFile ? get_by_base_id(dat, mat->materialFile) : 0;
            if (fnBase)
            {
                try
                {
                    std::vector<uint8_t> amatBytes = decomp(dat, fnBase - 1);
                    pkg = convertAmat(mdl::Extractor(amatBytes, tpl).extractAmatTree());
                }
                catch (const std::exception &e)
                {
                    pkg.error = e.what();
                }
            }
            else
            {
                pkg.error = "material has no AMAT file";
            }
            ai = amatByMaterial.emplace(g.materialIndex, std::move(pkg)).first;
        }
        const AmatPackage &pkg = ai->second;
        if (!pkg.ok())
        {
            std::fprintf(stderr, "material %u: %s\n", g.materialIndex, pkg.error.c_str());
            continue;
        }

        const int tech = amatSelectTechnique(pkg, maxQuality);
        // NOT the skinned feed, deliberately. The GPU-skinned variant is 1, and
        // its vertex shader reads the `grbones` matrix palette (see GrVsVariant);
        // this tool has no rig and uploads no palette, so binding it would skin
        // every vertex by whatever happened to be in the uniform.
        //
        // Passing 0x80 selects variant 2 instead, which is what this tool has
        // always drawn. The app's "Game 1:1" surface (src/render/gw2bgfx_view.cpp)
        // does pose the rig and does feed grbones, and asks for variant 1.
        const bool hasSkinFeed = (g.fvf & GR_FVF_WEIGHTS) && (g.fvf & GR_FVF_GROUP);
        const uint32_t variant = vsVariantFromMeshFlags(hasSkinFeed ? 0x80u : 0u, 0u, false);

        // Pass 0 is the one that paints; later passes need a depth prepass we
        // do not run.
        AmatSelection sel = amatSelectEffect(pkg, tech, 0, effectToken, variant);
        if (!sel.ok)
        {
            std::fprintf(stderr, "material %u: no effect for token 0x%llX\n",
                         g.materialIndex, (unsigned long long)effectToken);
            continue;
        }

        const AmatShaderBinary &vsBin = pkg.shaders[sel.vertexShaderIndex].dx11Shader;
        const AmatShaderBinary &psBin = pkg.shaders[sel.pixelShaderIndex].dx11Shader;

        // The blobs go to bgfx as stored. bgfx::copy because bgfx keeps the
        // memory until the shader is created and our vectors are about to go
        // out of scope.
        bgfx::ShaderHandle vsh = bgfx::createShader(bgfx::copy(vsBin.data.data(), (uint32_t)vsBin.data.size()));
        bgfx::ShaderHandle fsh = bgfx::createShader(bgfx::copy(psBin.data.data(), (uint32_t)psBin.data.size()));
        if (!bgfx::isValid(vsh) || !bgfx::isValid(fsh))
        {
            std::fprintf(stderr, "material %u: bgfx rejected a shader blob\n", g.materialIndex);
            continue;
        }

        Draw d;
        d.program = bgfx::createProgram(vsh, fsh, true);
        if (!bgfx::isValid(d.program))
        {
            std::fprintf(stderr, "material %u: program failed\n", g.materialIndex);
            continue;
        }

        d.vsU = parseBgfxBlobUniforms(vsBin.data);
        d.psU = parseBgfxBlobUniforms(psBin.data);
        for (const auto &u : d.vsU)
            uniformFor(u);
        for (const auto &u : d.psU)
            uniformFor(u);

        d.samplers = psBin.samplers;
        for (const auto &s : psBin.samplers)
        {
            bgfx::TextureHandle h;
            if (s.textureIndex < mat->textures.size())
                h = loadTexture(mat->textures[s.textureIndex].fileId);
            else if (s.textureSlot == 13)
                h = texCube; // environment cube
            else
                h = texWhite; // shadow map / light buffer
            d.textures.emplace_back((uint8_t)s.textureSlot, h);
        }

        // MODL material constants bind by NAME: the token32 decodes straight to
        // the uniform's name (base-23, not a hash). No positional pairing and
        // no alignment gate needed.
        for (const auto &c : mat->constants)
        {
            std::string name = tokenDecode32(c.name);
            if (!name.empty())
                d.matConsts[name] = Vec4{{c.value[0], c.value[1], c.value[2], c.value[3]}};
        }

        GrSurfaceState surf;
        surf.materialToken = effectToken;
        // surf.materialFlags is the RUNTIME word, `*(surface->material + 28)`,
        // which BgfxDraw_ComputeDepthState (0x140AB6D10) reads as a state
        // override mask: 0x200 forces DEPTH_TEST_GREATER, 0x800 kills the RGB
        // write mask, and so on. The MODL's ModelMaterialDataV*::materialFlags
        // is a different word in the same-sized slot -- a file-format field the
        // client translates at load time, not a value it submits. Feeding it in
        // here reads 0xA08 on every material of 291977, i.e. "no RGB write" plus
        // "depth test GREATER" against a depth buffer cleared to 1.0: nine draws,
        // 30k triangles, and not one pixel written.
        //
        // Zero is the correct stand-in until that translation is ported: it
        // means "no overrides", which for pass 0 leaves LEQUAL + depth write +
        // RGB|A write and takes cull from the effect's own pass flags.
        surf.materialFlags = 0;
        d.state = grComposeDrawState(*sel.effect, 0, surf).state;

        const bgfx::Memory *vmem = bgfx::copy(g.vertexBytes.data(), (uint32_t)g.vertexBytes.size());
        d.vb = bgfx::createVertexBuffer(vmem, layout);
        const bgfx::Memory *imem = bgfx::copy(g.indices.data(), (uint32_t)(g.indices.size() * 2));
        d.ib = bgfx::createIndexBuffer(imem);
        d.indexCount = (uint32_t)g.indices.size();
        draws.push_back(std::move(d));
    }

    std::printf("built %zu draws from %zu geosets\n", draws.size(), geosets.size());
    if (draws.empty())
    {
        bgfx::shutdown();
        return 1;
    }

    // --- frame loop ---------------------------------------------------------
    while (!g_quit)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit)
            break;

        // Follow the window. bgfx::reset re-creates the backbuffer at the new
        // size; without it the swap chain keeps the launch size and the driver
        // stretches it, which is the squashed-aspect look on any non-1280x800
        // window.
        if (g_sizeDirty)
        {
            bgfx::reset((uint32_t)g_width, (uint32_t)g_height, BGFX_RESET_VSYNC);
            g_sizeDirty = false;
        }

        // Fold the trackball into the world matrix, rotating about the model's
        // own centre.
        if (g_rotDirty)
        {
            float toOrigin[16], back[16], tmp[16], spin[16];
            bx::mtxTranslate(toOrigin, -centre[0], -centre[1], -centre[2]);
            bx::mtxTranslate(back, centre[0], centre[1], centre[2]);
            bx::mtxMul(tmp, toOrigin, g_rot);
            bx::mtxMul(spin, tmp, back);
            bx::mtxMul(world, base, spin);
            g_rotDirty = false;
        }

        // The camera does not move: it sits back along -Z with a constant +Y
        // up, and the trackball turns the model instead. That is why `up` can
        // be a constant -- there is no pole for it to flip across, which is
        // what made a long drag look like the model was jumping about.
        const float camDist = radius * g_distMul;
        const float eye[3] = {centre[0], centre[1], centre[2] - camDist};
        const float up[3] = {0.0f, 1.0f, 0.0f};

        float view[16], proj[16], viewProj[16];
        bx::mtxLookAt(view, bx::Vec3(eye[0], eye[1], eye[2]),
                      bx::Vec3(centre[0], centre[1], centre[2]),
                      bx::Vec3(up[0], up[1], up[2]));
        // Near/far bracketed to the model so the depth buffer keeps its
        // precision; a 1:2000 ratio crushes 24-bit depth into z-fighting.
        // Generous far plane: the trackball can swing a long model's far end
        // well past the centre, and clipping it looks like missing geometry.
        const float zn = std::max(camDist - radius * 2.0f, radius * 0.02f);
        const float zf = camDist + radius * 6.0f;
        const float aspect = float(g_width) / float(g_height > 0 ? g_height : 1);
        bx::mtxProj(proj, 60.0f, aspect, zn, zf, bgfx::getCaps()->homogeneousDepth);
        bx::mtxMul(viewProj, view, proj);

        float worldView[16];
        bx::mtxMul(worldView, world, view);

        // GW2's shaders want the transpose of what bx builds.
        //
        // bx's matrices are row-vector (`v * M`, translation in row 3), which is
        // bgfx's own convention and what its predefined u_modelViewProj expects.
        // GW2's HLSL multiplies the other way -- `mul(M, v)`, column-vector --
        // so the same 16 floats read as the transpose and every vertex lands
        // somewhere meaningless. Uploaded as bx builds them, the model is off
        // screen entirely: nine draws, 30k triangles, zero pixels touched.
        // Transposed, it frames exactly as the bounding sphere says it should.
        float viewProjT[16], worldT[16], worldViewT[16];
        bx::mtxTranspose(viewProjT, viewProj);
        bx::mtxTranspose(worldT, world);
        bx::mtxTranspose(worldViewT, worldView);

        bgfx::setViewRect(0, 0, 0, uint16_t(g_width), uint16_t(g_height));
        bgfx::touch(0);

        for (const Draw &d : draws)
        {
            // Per-uniform value lookup, in priority order: the transforms this
            // frame, then the material's own authored constants, then the
            // engine globals. A uniform none of them names is left alone.
            auto setAll = [&](const std::vector<BgfxBlobUniform> &list)
            {
                for (const auto &u : list)
                {
                    if (u.isSampler())
                        continue;
                    bgfx::UniformHandle h = uniformFor(u);
                    if (u.name == "ViewProjection")
                    {
                        bgfx::setUniform(h, viewProjT);
                        continue;
                    }
                    if (u.name == "World")
                    {
                        bgfx::setUniform(h, worldT);
                        continue;
                    }
                    if (u.name == "WorldView")
                    {
                        bgfx::setUniform(h, worldViewT);
                        continue;
                    }
                    if (u.name == "CameraPosition")
                    {
                        const float v[4] = {eye[0], eye[1], eye[2], 1.0f};
                        bgfx::setUniform(h, v);
                        continue;
                    }
                    if (u.name == "Time")
                    {
                        const float t = float(GetTickCount64() % 100000) * 0.001f;
                        const float v[4] = {t, t, t, t};
                        bgfx::setUniform(h, v);
                        continue;
                    }
                    auto mc = d.matConsts.find(u.name);
                    if (mc != d.matConsts.end())
                    {
                        bgfx::setUniform(h, mc->second.v);
                        continue;
                    }
                    auto eg = kEngineUniforms.find(u.name);
                    if (eg != kEngineUniforms.end())
                    {
                        bgfx::setUniform(h, eg->second.v);
                        continue;
                    }
                }
            };
            setAll(d.vsU);
            setAll(d.psU);

            // Textures bind to the sampler uniform whose name the shader gave
            // that stage -- gSs13, ss0 and so on. The slot number is the one
            // the AMAT recorded, which is also what the name encodes.
            for (size_t i = 0; i < d.textures.size(); ++i)
            {
                const uint8_t slot = d.textures[i].first;
                for (const auto &u : d.psU)
                {
                    if (!u.isSampler())
                        continue;
                    if (u.regIndex != slot)
                        continue;
                    bgfx::setTexture(slot, uniformFor(u), d.textures[i].second);
                    break;
                }
            }

            bgfx::setVertexBuffer(0, d.vb);
            bgfx::setIndexBuffer(d.ib, 0, d.indexCount);
            bgfx::setState(d.state);
            bgfx::submit(0, d.program, 0, BGFX_DISCARD_ALL);
        }

        bgfx::frame();
    }

    bgfx::shutdown();
    return 0;
}
