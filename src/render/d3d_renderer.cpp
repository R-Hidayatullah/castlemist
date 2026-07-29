#include "castlemist/render/d3d_renderer.h"

#include <algorithm>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace castlemist::gfx {

namespace {

using Microsoft::WRL::ComPtr;

HWND g_hwnd = nullptr;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swapchain;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11VertexShader> g_vertex_shader;
ComPtr<ID3D11PixelShader> g_pixel_shader;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11ShaderResourceView> g_texture_view;
ComPtr<ID3D11Buffer> g_view_cbuffer;
// Streaming surface for video playback (see begin_video/update_video).
ComPtr<ID3D11Texture2D> g_video_texture;
uint32_t g_video_width = 0;
uint32_t g_video_height = 0;

uint32_t g_tex_width = 0;
uint32_t g_tex_height = 0;
bool g_has_texture = false;
int g_client_width = 1;
int g_client_height = 1;
float g_zoom = 1.0f;
float g_pan_x = 0.0f;
float g_pan_y = 0.0f;
int g_rotation_quarters = 0; // 0-3, stored separately from the radians below for exact odd/even checks
bool g_alpha_aware = true;    // true: checkerboard composite; false: raw RGB (ignore alpha)

struct ViewParams {
    float zoom;
    float pan_x;
    float pan_y;
    float rotation; // radians
    float alpha_mode;
    float pad[3];
};

constexpr float kPi = 3.14159265358979323846f;

// Fullscreen-quad shader: vertex positions/UVs are generated purely from
// SV_VertexID, so no vertex buffer or input layout is needed at all. The
// pixel shader remaps UVs by the zoom/pan constant buffer before sampling.
constexpr char kShaderSource[] = R"(
cbuffer ViewParams : register(b0) {
    float zoom;
    float panX;
    float panY;
    float rotation;
    float alphaMode;   // 1 = alpha-aware (checkerboard); 0 = raw RGB (ignore alpha)
    float3 _pad;
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 pos[4] = { float2(-1,-1), float2(-1,1), float2(1,-1), float2(1,1) };
    float2 uv[4]  = { float2(0,1),   float2(0,0),  float2(1,1),  float2(1,0) };
    VSOut o;
    o.pos = float4(pos[id], 0.0, 1.0);
    o.uv = uv[id];
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSMain(VSOut input) : SV_TARGET {
    float2 centered = input.uv - 0.5;
    float s = sin(rotation);
    float c = cos(rotation);
    float2 rotated = float2(centered.x * c - centered.y * s, centered.x * s + centered.y * c);
    float2 uv = 0.5 + rotated / max(zoom, 0.0001) + float2(panX, panY);
    float4 texel = tex0.Sample(samp0, uv);
    // Raw-RGB mode (alpha toggle off): show the colour channels as-is, ignoring
    // alpha. Useful for inspecting the RGB data of masks/data textures.
    if (alphaMode < 0.5)
        return float4(texel.rgb, 1.0);
    // Alpha-aware mode (default): composite over a checkerboard using the alpha
    // channel. Many ArenaNet textures (e.g. mostly-transparent ATEP decals) store
    // a don't-care colour under fully-transparent texels -- often flat red.
    // Sampling RGB opaquely would show that red; honoring alpha shows the texture
    // as it is actually used. Fully-opaque texels (alpha==1) are unaffected.
    float2 cb = floor(input.pos.xy / 8.0);
    float chk = (fmod(cb.x + cb.y, 2.0) < 1.0) ? 0.62 : 0.40;
    float3 rgb = lerp(float3(chk, chk, chk), texel.rgb, texel.a);
    return float4(rgb, 1.0);
}
)";

void create_render_target() {
    g_rtv.Reset();
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        return;
    }
    g_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &g_rtv);
}

D3D11_VIEWPORT compute_letterbox_viewport() {
    float win_w = static_cast<float>(std::max(1, g_client_width));
    float win_h = static_cast<float>(std::max(1, g_client_height));

    // A 90/270 rotation swaps which texture dimension maps to the on-screen
    // width vs. height, so the letterbox aspect ratio must swap with it.
    bool rotated_sideways = (g_rotation_quarters % 2) != 0;
    uint32_t effective_tex_w = rotated_sideways ? g_tex_height : g_tex_width;
    uint32_t effective_tex_h = rotated_sideways ? g_tex_width : g_tex_height;
    float tex_aspect = static_cast<float>(effective_tex_w) / static_cast<float>(std::max<uint32_t>(1, effective_tex_h));
    float win_aspect = win_w / win_h;

    float vp_w;
    float vp_h;
    if (win_aspect > tex_aspect) {
        vp_h = win_h;
        vp_w = vp_h * tex_aspect;
    } else {
        vp_w = win_w;
        vp_h = vp_w / tex_aspect;
    }

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (win_w - vp_w) * 0.5f;
    vp.TopLeftY = (win_h - vp_h) * 0.5f;
    vp.Width = vp_w;
    vp.Height = vp_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    return vp;
}

bool create_device_and_swapchain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = static_cast<UINT>(g_client_width);
    scd.BufferDesc.Height = static_cast<UINT>(g_client_height);
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                                ARRAYSIZE(levels), D3D11_SDK_VERSION, &scd, &g_swapchain, &g_device,
                                                &obtained, &g_context);
    if (FAILED(hr)) {
        // Fall back to the software rasterizer so the app still runs on a
        // machine without a usable GPU driver.
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, ARRAYSIZE(levels),
                                            D3D11_SDK_VERSION, &scd, &g_swapchain, &g_device, &obtained, &g_context);
    }
    return SUCCEEDED(hr);
}

bool create_shaders() {
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> error_blob;

    HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "castlemist_image_shader", nullptr, nullptr, "VSMain",
                             "vs_4_0", 0, 0, &vs_blob, &error_blob);
    if (FAILED(hr)) {
        return false;
    }

    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "castlemist_image_shader", nullptr, nullptr, "PSMain", "ps_4_0",
                     0, 0, &ps_blob, &error_blob);
    if (FAILED(hr)) {
        return false;
    }

    hr = g_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_vertex_shader);
    if (FAILED(hr)) {
        return false;
    }

    hr = g_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_pixel_shader);
    return SUCCEEDED(hr);
}

bool create_sampler() {
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(g_device->CreateSamplerState(&desc, &g_sampler));
}

bool create_view_cbuffer() {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(ViewParams);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(g_device->CreateBuffer(&desc, nullptr, &g_view_cbuffer));
}

void upload_view_cbuffer() {
    if (!g_context || !g_view_cbuffer) {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_context->Map(g_view_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ViewParams params{g_zoom, g_pan_x, g_pan_y, static_cast<float>(g_rotation_quarters) * (kPi * 0.5f),
                          g_alpha_aware ? 1.0f : 0.0f, {0.0f, 0.0f, 0.0f}};
        std::memcpy(mapped.pData, &params, sizeof(params));
        g_context->Unmap(g_view_cbuffer.Get(), 0);
    }
}

} // namespace

bool initialize(HWND target_window) {
    g_hwnd = target_window;

    RECT rc;
    GetClientRect(target_window, &rc);
    g_client_width = std::max<int>(1, rc.right - rc.left);
    g_client_height = std::max<int>(1, rc.bottom - rc.top);

    if (!create_device_and_swapchain(target_window)) {
        return false;
    }

    create_render_target();

    if (!create_shaders() || !create_sampler() || !create_view_cbuffer()) {
        return false;
    }

    return true;
}

void shutdown() {
    g_view_cbuffer.Reset();
    g_video_texture.Reset();
    g_texture_view.Reset();
    g_sampler.Reset();
    g_pixel_shader.Reset();
    g_vertex_shader.Reset();
    g_rtv.Reset();
    g_swapchain.Reset();
    g_context.Reset();
    g_device.Reset();
    g_hwnd = nullptr;
}

void on_resize(int width, int height) {
    if (!g_swapchain || width <= 0 || height <= 0) {
        return;
    }

    g_client_width = width;
    g_client_height = height;

    g_rtv.Reset();
    g_swapchain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    create_render_target();
}

void set_texture(uint32_t dxgi_format, uint32_t width, uint32_t height, uint32_t sys_mem_pitch, const uint8_t* data) {
    g_texture_view.Reset();
    g_has_texture = false;
    // A still image replaces any video streaming surface that was up.
    g_video_texture.Reset();
    g_video_width = 0;
    g_video_height = 0;

    if (!g_device || data == nullptr || width == 0 || height == 0) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(dxgi_format);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = data;
    sub.SysMemPitch = sys_mem_pitch;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(g_device->CreateTexture2D(&desc, &sub, &texture))) {
        return;
    }

    if (FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &g_texture_view))) {
        return;
    }

    g_tex_width = width;
    g_tex_height = height;
    g_has_texture = true;
    g_rotation_quarters = 0; // a newly selected image always starts unrotated
    reset_view();
}

void clear_texture() {
    g_texture_view.Reset();
    g_has_texture = false;
    g_video_texture.Reset();
    g_video_width = 0;
    g_video_height = 0;
}

bool begin_video(uint32_t width, uint32_t height) {
    if (!g_device || width == 0 || height == 0) {
        return false;
    }
    if (g_video_texture && g_video_width == width && g_video_height == height) {
        // Same movie dimensions as the last one: keep the resource and its view.
        g_has_texture = true;
        return true;
    }

    g_texture_view.Reset();
    g_video_texture.Reset();
    g_has_texture = false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // matches Bink's BINKSURFACE32BGRA/x
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_video_texture))) {
        return false;
    }
    if (FAILED(g_device->CreateShaderResourceView(g_video_texture.Get(), nullptr, &g_texture_view))) {
        g_video_texture.Reset();
        return false;
    }

    g_video_width = width;
    g_video_height = height;
    g_tex_width = width;
    g_tex_height = height;
    g_has_texture = true;
    g_rotation_quarters = 0;
    reset_view();
    return true;
}

void update_video(const uint8_t* bgra, uint32_t pitch) {
    if (!g_context || !g_video_texture || bgra == nullptr || pitch == 0) {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_video_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    // D3D hands back its own row pitch, which is rarely the frame's own -- copy
    // row by row unless they happen to agree.
    if (mapped.RowPitch == pitch) {
        std::memcpy(mapped.pData, bgra, static_cast<size_t>(pitch) * g_video_height);
    } else {
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        const uint32_t row = std::min<uint32_t>(pitch, mapped.RowPitch);
        for (uint32_t y = 0; y < g_video_height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch, bgra + static_cast<size_t>(y) * pitch, row);
        }
    }
    g_context->Unmap(g_video_texture.Get(), 0);
    g_has_texture = true;
}

void set_view(float zoom, float pan_x, float pan_y) {
    g_zoom = std::clamp(zoom, 0.05f, 40.0f);
    g_pan_x = pan_x;
    g_pan_y = pan_y;
}

void reset_view() {
    g_zoom = 1.0f;
    g_pan_x = 0.0f;
    g_pan_y = 0.0f;
}

void set_rotation(int quarter_turns) {
    int normalized = quarter_turns % 4;
    if (normalized < 0) {
        normalized += 4;
    }
    g_rotation_quarters = normalized;
}

void set_alpha_aware(bool on) {
    g_alpha_aware = on;
}

bool alpha_aware() {
    return g_alpha_aware;
}

void render() {
    if (!g_context || !g_rtv || !g_swapchain) {
        return;
    }

    const float background[4] = {0.10f, 0.10f, 0.12f, 1.0f};
    ID3D11RenderTargetView* rtvs[] = {g_rtv.Get()};
    g_context->OMSetRenderTargets(1, rtvs, nullptr);
    g_context->ClearRenderTargetView(g_rtv.Get(), background);

    if (g_has_texture && g_texture_view) {
        D3D11_VIEWPORT viewport = compute_letterbox_viewport();
        g_context->RSSetViewports(1, &viewport);

        upload_view_cbuffer();

        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_context->VSSetShader(g_vertex_shader.Get(), nullptr, 0);
        g_context->PSSetShader(g_pixel_shader.Get(), nullptr, 0);

        ID3D11Buffer* cbuffers[] = {g_view_cbuffer.Get()};
        g_context->PSSetConstantBuffers(0, 1, cbuffers);

        ID3D11ShaderResourceView* srvs[] = {g_texture_view.Get()};
        g_context->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samplers[] = {g_sampler.Get()};
        g_context->PSSetSamplers(0, 1, samplers);

        g_context->Draw(4, 0);
    }

    g_swapchain->Present(1, 0);
}

} // namespace castlemist::gfx
