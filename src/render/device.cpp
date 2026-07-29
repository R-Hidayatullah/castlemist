/// @file
/// @brief Direct3D 11 device, swap chain, render targets and the shared pipeline objects.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>

#include "detail/shaders.h"

namespace castlemist::render {

void create_targets() {
    g_rtv.Reset();
    g_dsv.Reset();
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    g_dev->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = static_cast<UINT>(g_w);
    dd.Height = static_cast<UINT>(g_h);
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> ds;
    if (SUCCEEDED(g_dev->CreateTexture2D(&dd, nullptr, &ds))) {
        g_dev->CreateDepthStencilView(ds.Get(), nullptr, &g_dsv);
    }

    // Offscreen colour target the GameShader pass renders into. 8-bit UNORM, matching
    // the game: its material pass writes to a B8G8R8A8 target, so material output is
    // CLAMPED to [0,1] on write. Keeping float headroom here (it was RGBA16F) and
    // recovering it with a tonemap is what made the preview diverge -- the game has no
    // such headroom and no tonemap. See kGameLightDecode in state.h.
    g_sceneTex.Reset(); g_sceneRTV.Reset(); g_sceneSRV.Reset();
    D3D11_TEXTURE2D_DESC cd{};
    cd.Width = static_cast<UINT>(g_w);
    cd.Height = static_cast<UINT>(g_h);
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // headroom for the exposure/knee pass
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(g_dev->CreateTexture2D(&cd, nullptr, &g_sceneTex))) {
        g_dev->CreateRenderTargetView(g_sceneTex.Get(), nullptr, &g_sceneRTV);
        g_dev->CreateShaderResourceView(g_sceneTex.Get(), nullptr, &g_sceneSRV);
    }

    // Light pre-pass targets: a world-normal G-buffer (RGBA8) and the screen-space
    // light-accumulation buffer bound as gSs14. The light buffer keeps float storage
    // so it can hold the >1 values a lit surface reaches (the game's own buffer peaks
    // at 1.28 decoded) without the 0.25/4.0 storage encoding -- see kLightBufferDecode
    // in state.h for why reproducing that encoding is wrong for us.
    g_nrmTex.Reset(); g_nrmRTV.Reset(); g_nrmSRV.Reset();
    g_lightTex.Reset(); g_lightRTV.Reset(); g_lightSRV.Reset();
    D3D11_TEXTURE2D_DESC nd{};
    nd.Width = static_cast<UINT>(g_w);
    nd.Height = static_cast<UINT>(g_h);
    nd.MipLevels = 1;
    nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nd.SampleDesc.Count = 1;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(g_dev->CreateTexture2D(&nd, nullptr, &g_nrmTex))) {
        g_dev->CreateRenderTargetView(g_nrmTex.Get(), nullptr, &g_nrmRTV);
        g_dev->CreateShaderResourceView(g_nrmTex.Get(), nullptr, &g_nrmSRV);
    }
    D3D11_TEXTURE2D_DESC ld = nd;
    ld.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (SUCCEEDED(g_dev->CreateTexture2D(&ld, nullptr, &g_lightTex))) {
        g_dev->CreateRenderTargetView(g_lightTex.Get(), nullptr, &g_lightRTV);
        g_dev->CreateShaderResourceView(g_lightTex.Get(), nullptr, &g_lightSRV);
    }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sc{};
    sc.BufferCount = 2;
    sc.BufferDesc.Width = static_cast<UINT>(g_w);
    sc.BufferDesc.Height = static_cast<UINT>(g_h);
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow = hwnd;
    sc.SampleDesc.Count = 1;
    sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                                               D3D11_SDK_VERSION, &sc, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, ARRAYSIZE(levels),
                                           D3D11_SDK_VERSION, &sc, &g_swap, &g_dev, &got, &g_ctx);
    }
    return SUCCEEDED(hr);
}

// Fullscreen post-process: brighten the (correct but dark) game-shader output.
// VS builds a fullscreen triangle from SV_VertexID (no vertex buffer); PS applies
// exposure, a small ambient lift so crushed shadows regain detail, then a >1
// gamma to lift midtones. Kept LDR-safe with saturate().

bool init_pipeline() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(shaders::kModel, std::strlen(shaders::kModel), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)))
        return false;
    if (FAILED(D3DCompile(shaders::kModel, std::strlen(shaders::kModel), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err)))
        return false;
    if (FAILED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs))) return false;
    if (FAILED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps))) return false;

    // Post-process shaders + its 16-byte cbuffer (best-effort; darkness fix only).
    {
        if (const char* e = std::getenv("GW2_POSTEXP"))   { double v = std::atof(e); if (v > 0.01) { g_post_exp = (float)v; g_post_exp_env = true; } }
        if (const char* e = std::getenv("GW2_POSTAMB"))   { double v = std::atof(e); if (v >= 0)   g_post_amb = (float)v; }
        if (const char* e = std::getenv("GW2_POSTGAMMA")) { double v = std::atof(e); if (v > 0.05) g_post_gamma = (float)v; }
        ComPtr<ID3DBlob> pvs, pps, perr;
        if (SUCCEEDED(D3DCompile(shaders::kPost, std::strlen(shaders::kPost), nullptr, nullptr, nullptr, "VSPost", "vs_5_0", 0, 0, &pvs, &perr)) &&
            SUCCEEDED(D3DCompile(shaders::kPost, std::strlen(shaders::kPost), nullptr, nullptr, nullptr, "PSPost", "ps_5_0", 0, 0, &pps, &perr)) &&
            SUCCEEDED(g_dev->CreateVertexShader(pvs->GetBufferPointer(), pvs->GetBufferSize(), nullptr, &g_postVS)) &&
            SUCCEEDED(g_dev->CreatePixelShader(pps->GetBufferPointer(), pps->GetBufferSize(), nullptr, &g_postPS))) {
            D3D11_BUFFER_DESC pd{};
            pd.Usage = D3D11_USAGE_DYNAMIC;
            pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            pd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            pd.ByteWidth = 16;
            if (SUCCEEDED(g_dev->CreateBuffer(&pd, nullptr, &g_postCB))) g_post_ready = true;
        }
    }

    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 128, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g_dev->CreateInputLayout(il, 7, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = (sizeof(CB) + 15) & ~15u;
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb))) return false;

    // Bone palette (StructuredBuffer) is created per-model in build_skeleton once
    // the bone count is known -- no fixed-size allocation here.

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_samp);

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rs, &g_rsSolid);
    rs.FillMode = D3D11_FILL_WIREFRAME;
    g_dev->CreateRasterizerState(&rs, &g_rsWire);
    // Single-sided solids for the two-pass transparency draw. GW2 winds triangles
    // so CULL_BACK keeps the front (camera-facing) facets; CULL_FRONT keeps the back.
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_FRONT;
    g_dev->CreateRasterizerState(&rs, &g_rsCullFront);
    rs.CullMode = D3D11_CULL_BACK;
    g_dev->CreateRasterizerState(&rs, &g_rsCullBack);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    g_dev->CreateDepthStencilState(&ds, &g_dss);
    D3D11_DEPTH_STENCIL_DESC dsn = ds;
    dsn.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    g_dev->CreateDepthStencilState(&dsn, &g_dssNoWrite);
    D3D11_DEPTH_STENCIL_DESC dsk{};
    dsk.DepthEnable = FALSE; // skeleton always visible, drawn over the mesh
    g_dev->CreateDepthStencilState(&dsk, &g_dssNoDepth);

    // Skeleton overlay pipeline (position-only line list).
    ComPtr<ID3DBlob> lvsb, lpsb, lerr;
    if (SUCCEEDED(D3DCompile(shaders::kLine, std::strlen(shaders::kLine), nullptr, nullptr, nullptr, "VSLine", "vs_5_0", 0, 0,
                             &lvsb, &lerr)) &&
        SUCCEEDED(D3DCompile(shaders::kLine, std::strlen(shaders::kLine), nullptr, nullptr, nullptr, "PSLine", "ps_5_0", 0, 0,
                             &lpsb, &lerr))) {
        g_dev->CreateVertexShader(lvsb->GetBufferPointer(), lvsb->GetBufferSize(), nullptr, &g_lineVs);
        g_dev->CreatePixelShader(lpsb->GetBufferPointer(), lpsb->GetBufferSize(), nullptr, &g_linePs);
        const D3D11_INPUT_ELEMENT_DESC lil[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        g_dev->CreateInputLayout(lil, 1, lvsb->GetBufferPointer(), lvsb->GetBufferSize(), &g_lineIl);
    }

    // Gizmo / grid / world-axis pipeline (position + per-vertex color line list).
    ComPtr<ID3DBlob> gvsb, gpsb, gerr;
    if (SUCCEEDED(D3DCompile(shaders::kGizmo, std::strlen(shaders::kGizmo), nullptr, nullptr, nullptr, "VSGiz", "vs_5_0", 0, 0,
                             &gvsb, &gerr)) &&
        SUCCEEDED(D3DCompile(shaders::kGizmo, std::strlen(shaders::kGizmo), nullptr, nullptr, nullptr, "PSGiz", "ps_5_0", 0, 0,
                             &gpsb, &gerr))) {
        g_dev->CreateVertexShader(gvsb->GetBufferPointer(), gvsb->GetBufferSize(), nullptr, &g_gizVs);
        g_dev->CreatePixelShader(gpsb->GetBufferPointer(), gpsb->GetBufferSize(), nullptr, &g_gizPs);
        const D3D11_INPUT_ELEMENT_DESC gil[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        g_dev->CreateInputLayout(gil, 2, gvsb->GetBufferPointer(), gvsb->GetBufferSize(), &g_gizIl);
    }

    D3D11_BLEND_DESC bo{};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bo, &g_blendOpaque);
    D3D11_BLEND_DESC ba{};
    auto& rt = ba.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&ba, &g_blendAlpha);

    // ---- light pre-pass shaders + sun/ambient cbuffer (Option 2 lighting) ----
    if (const char* e = std::getenv("GW2_LIGHTPREPASS")) g_lightprepass_on = std::atoi(e) != 0;
    {
        ComPtr<ID3DBlob> nvsb, npsb, lvsb2, lpsb2, lerr2;
        if (SUCCEEDED(D3DCompile(shaders::kNormalGBuffer, std::strlen(shaders::kNormalGBuffer), nullptr, nullptr, nullptr, "VSNrm", "vs_5_0", 0, 0, &nvsb, &lerr2)) &&
            SUCCEEDED(D3DCompile(shaders::kNormalGBuffer, std::strlen(shaders::kNormalGBuffer), nullptr, nullptr, nullptr, "PSNrm", "ps_5_0", 0, 0, &npsb, &lerr2)) &&
            SUCCEEDED(D3DCompile(shaders::kLightAccum, std::strlen(shaders::kLightAccum), nullptr, nullptr, nullptr, "VSFS", "vs_5_0", 0, 0, &lvsb2, &lerr2)) &&
            SUCCEEDED(D3DCompile(shaders::kLightAccum, std::strlen(shaders::kLightAccum), nullptr, nullptr, nullptr, "PSLight", "ps_5_0", 0, 0, &lpsb2, &lerr2))) {
            g_dev->CreateVertexShader(nvsb->GetBufferPointer(), nvsb->GetBufferSize(), nullptr, &g_nrmVs);
            g_dev->CreatePixelShader(npsb->GetBufferPointer(), npsb->GetBufferSize(), nullptr, &g_nrmPs);
            g_dev->CreateVertexShader(lvsb2->GetBufferPointer(), lvsb2->GetBufferSize(), nullptr, &g_lightVs);
            g_dev->CreatePixelShader(lpsb2->GetBufferPointer(), lpsb2->GetBufferSize(), nullptr, &g_lightPs);
        }
        // The light cbuffer now carries the SH rig (shRed/shGreen/shBlue/shSun/
        // shSunColor = 5 float4 = 80 bytes) and is refilled per frame in
        // render_light_prepass() straight from g_game_vals -- the identical values
        // the forward materials bind -- so both paths share one lighting model.
        D3D11_BUFFER_DESC lbd{};
        lbd.Usage = D3D11_USAGE_DYNAMIC;
        lbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        lbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        lbd.ByteWidth = 96; // 6 x float4 (SH rig + light-encode params)
        g_dev->CreateBuffer(&lbd, nullptr, &g_lightCB);
    }

    // ---- particle billboard pipeline ----
    if (const char* e = std::getenv("GW2_EFFECTS")) g_show_effects = std::atoi(e) != 0;
    if (const char* e = std::getenv("GW2_FXINTENSITY")) { float v = (float)std::atof(e); if (v > 0) g_fxIntensity = v; }
    if (const char* e = std::getenv("GW2_WIND")) g_fxWindGust = (float)std::atof(e);
    if (const char* e = std::getenv("GW2_WINDDIR")) { float x,y,z; if (sscanf(e, "%f,%f,%f", &x,&y,&z) == 3) g_fxWindDir = norm({x,y,z}); }
    {
        ComPtr<ID3DBlob> pvsb, ppsb, perr;
        if (SUCCEEDED(D3DCompile(shaders::kParticle, std::strlen(shaders::kParticle), nullptr, nullptr, nullptr, "VSPart", "vs_5_0", 0, 0, &pvsb, &perr)) &&
            SUCCEEDED(D3DCompile(shaders::kParticle, std::strlen(shaders::kParticle), nullptr, nullptr, nullptr, "PSPart", "ps_5_0", 0, 0, &ppsb, &perr))) {
            g_dev->CreateVertexShader(pvsb->GetBufferPointer(), pvsb->GetBufferSize(), nullptr, &g_partVs);
            g_dev->CreatePixelShader(ppsb->GetBufferPointer(), ppsb->GetBufferSize(), nullptr, &g_partPs);
            D3D11_INPUT_ELEMENT_DESC il[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
            };
            g_dev->CreateInputLayout(il, 4, pvsb->GetBufferPointer(), pvsb->GetBufferSize(), &g_partIl);
        }
        D3D11_BUFFER_DESC cbd{};
        cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; cbd.ByteWidth = 64;
        g_dev->CreateBuffer(&cbd, nullptr, &g_partCb);
        // Additive (glow) blend: PS already outputs premultiplied color.
        D3D11_BLEND_DESC bd{};
        auto& rt2 = bd.RenderTarget[0];
        rt2.BlendEnable = TRUE;
        rt2.SrcBlend = D3D11_BLEND_ONE;  rt2.DestBlend = D3D11_BLEND_ONE;  rt2.BlendOp = D3D11_BLEND_OP_ADD;
        rt2.SrcBlendAlpha = D3D11_BLEND_ONE; rt2.DestBlendAlpha = D3D11_BLEND_ONE; rt2.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt2.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &g_blendAdd);
        // Procedural soft radial sprite (used when a cloud material has no texture,
        // and for effect lights): white with a smooth alpha falloff to the edge.
        const int S = 64; std::vector<uint8_t> disc(S * S * 4);
        for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
            float dx = (x + 0.5f) / S * 2 - 1, dy = (y + 0.5f) / S * 2 - 1;
            float d = std::sqrt(dx * dx + dy * dy);
            float a = std::clamp(1.0f - d, 0.0f, 1.0f); a *= a; // soft falloff
            uint8_t* p = &disc[(y * S + x) * 4];
            p[0] = p[1] = p[2] = 255; p[3] = (uint8_t)(a * 255);
        }
        g_softDisc = make_srv(disc, S, S);
    }
    return true;
}

ComPtr<ID3D11ShaderResourceView> make_srv(const std::vector<uint8_t>& px, int w, int h) {
    ComPtr<ID3D11ShaderResourceView> out;
    if (px.empty() || w <= 0 || h <= 0) return out;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = px.data();
    sd.SysMemPitch = static_cast<UINT>(w) * 4;
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex))) {
        g_dev->CreateShaderResourceView(tex.Get(), nullptr, &out);
    }
    return out;
}

// Half-resolution SRV: box-downsamples the RGBA to w/2 x h/2 and uploads it. GW2's
// "reduced" texture sibling is essentially the half-size version, so this is the
// per-submesh "reduced texture" option without a second dat decode. Falls back to a
// full-res SRV when the texture is already tiny.
ComPtr<ID3D11ShaderResourceView> make_srv_half(const std::vector<uint8_t>& px, int w, int h) {
    if (px.empty() || w <= 2 || h <= 2) return make_srv(px, w, h);
    int hw = w / 2, hh = h / 2;
    std::vector<uint8_t> out((size_t)hw * hh * 4);
    for (int y = 0; y < hh; ++y) {
        for (int x = 0; x < hw; ++x) {
            int sx = x * 2, sy = y * 2;
            for (int c = 0; c < 4; ++c) {
                int s = px[((size_t)sy * w + sx) * 4 + c] + px[((size_t)sy * w + sx + 1) * 4 + c] +
                        px[((size_t)(sy + 1) * w + sx) * 4 + c] + px[((size_t)(sy + 1) * w + sx + 1) * 4 + c];
                out[((size_t)y * hw + x) * 4 + c] = (uint8_t)(s >> 2);
            }
        }
    }
    return make_srv(out, hw, hh);
}


ComPtr<ID3D11ShaderResourceView> make_solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> px = {r, g, b, a};
    return make_srv(px, 1, 1);
}
// 1x1x6 constant-color CUBE SRV -- stand-in for the GLOBAL env cubemap (slot 13).
// Must be an actual cube (the PS declares texturecube); a mismatched 2D samples
// white -> mirror-white metals. Neutral grey gives believable reflections.
ComPtr<ID3D11ShaderResourceView> make_solid_cube(uint8_t lv) {
    uint8_t px[4] = {lv, lv, lv, 255};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SUBRESOURCE_DATA sd[6];
    for (int i = 0; i < 6; i++) { sd[i].pSysMem = px; sd[i].SysMemPitch = 4; sd[i].SysMemSlicePitch = 4; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = td.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; vd.TextureCube.MipLevels = 1;
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> out;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, sd, &tex))) g_dev->CreateShaderResourceView(tex.Get(), &vd, &out);
    return out;
}

// bgfx 64-bit state blend nibble -> D3D11_BLEND (shifts 12/16/20/24).
// Exact bgfx blend-factor table (renderer_d3d11.cpp s_blendFactor). Column 0 is the
// RGB channel, column 1 the ALPHA channel -- D3D11 forbids *_COLOR factors on the
// alpha channel, so bgfx remaps them to the *_ALPHA equivalents there.

bool initialize(HWND target_window) {
    g_hwnd = target_window;
    RECT rc;
    GetClientRect(target_window, &rc);
    g_w = std::max<int>(1, rc.right - rc.left);
    g_h = std::max<int>(1, rc.bottom - rc.top);
    if (!create_device(target_window)) return false;
    create_targets();
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_last);
    QueryPerformanceCounter(&g_qpc_start);
    return init_pipeline();
}

void shutdown() {
    clear_model();
    g_bonePaletteSRV.Reset();
    g_bonePalette.Reset();
    g_bone_cap = 0;
    g_lineIl.Reset(); g_linePs.Reset(); g_lineVs.Reset(); g_dssNoDepth.Reset();
    g_blendAlpha.Reset(); g_blendOpaque.Reset();
    g_dssNoWrite.Reset(); g_dss.Reset();
    g_rsWire.Reset(); g_rsSolid.Reset(); g_rsCullFront.Reset(); g_rsCullBack.Reset();
    g_samp.Reset(); g_cb.Reset(); g_il.Reset(); g_ps.Reset(); g_vs.Reset();
    g_postCB.Reset(); g_postPS.Reset(); g_postVS.Reset();
    g_lightCB.Reset(); g_lightPs.Reset(); g_lightVs.Reset(); g_nrmPs.Reset(); g_nrmVs.Reset();
    g_lightSRV.Reset(); g_lightRTV.Reset(); g_lightTex.Reset();
    g_nrmSRV.Reset(); g_nrmRTV.Reset(); g_nrmTex.Reset();
    g_sceneSRV.Reset(); g_sceneRTV.Reset(); g_sceneTex.Reset(); g_post_ready = false;
    g_dsv.Reset(); g_rtv.Reset(); g_swap.Reset(); g_ctx.Reset(); g_dev.Reset();
    g_hwnd = nullptr;
}

void on_resize(int width, int height) {
    if (!g_swap || width <= 0 || height <= 0) return;
    g_w = width;
    g_h = height;
    g_rtv.Reset();
    g_dsv.Reset();
    g_swap->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    create_targets();
}

// particle system (defined below, near render())

LONGLONG now_qpc() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

// CPU linear-blend skinning of the rest mesh (g_cpu_verts) into `dst` using the
// current pose. GW2's material VS take pre-skinned vertices, so this is how the
// game-shader path animates a rig. Positions + tangent frame are skinned; UVs
// and bone data are copied through. Returns false if there's nothing to skin.

} // namespace castlemist::render
