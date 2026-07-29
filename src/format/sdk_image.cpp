// sdk_image.cpp -- OG-library-first still-image decoding (see sdk_image.h).
// This is the single translation unit that instantiates stb_image.
#include "castlemist/format/sdk_image.h"

#include "castlemist/format/wic_image.h"

#include <cstring>
#include <csetjmp>

// ---- original reference libraries ------------------------------------------
extern "C" {
#include <jpeglib.h>     // libjpeg-turbo 3.2.0 (third_party/jpegcfg holds jconfig.h)
#include <jerror.h>
#include <webp/decode.h> // libwebp 1.6.0
}

// ---- last-resort fallback ---------------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
// stb is only ever the fallback, so drop the codecs the OG libs already own.
#define STBI_NO_JPEG
#include "stb_image.h"

namespace castlemist::img {
namespace {

// Expand any component count stb/libjpeg may hand back into RGBA8888.
void to_rgba(const uint8_t* src, int comp, size_t pixels, std::vector<uint8_t>& dst) {
    dst.resize(pixels * 4);
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t r, g, b, a = 255;
        switch (comp) {
        case 1: r = g = b = src[i]; break;
        case 2: r = g = b = src[i * 2]; a = src[i * 2 + 1]; break;
        case 3: r = src[i * 3]; g = src[i * 3 + 1]; b = src[i * 3 + 2]; break;
        default: r = src[i * 4]; g = src[i * 4 + 1]; b = src[i * 4 + 2]; a = src[i * 4 + 3]; break;
        }
        dst[i * 4] = r; dst[i * 4 + 1] = g; dst[i * 4 + 2] = b; dst[i * 4 + 3] = a;
    }
}

// --- libjpeg-turbo -----------------------------------------------------------
// libjpeg signals fatal errors by longjmp'ing out of error_exit; without this the
// default handler would call exit() and kill the browser on a corrupt entry.
struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

void jpeg_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErr*>(cinfo->err);
    std::longjmp(err->setjmp_buffer, 1);
}
void jpeg_output_silent(j_common_ptr) {}  // swallow warnings

std::optional<Image> decode_jpeg_turbo(const uint8_t* d, size_t n) {
    jpeg_decompress_struct cinfo{};
    JpegErr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    jerr.pub.output_message = jpeg_output_silent;

    std::vector<uint8_t> pixels;
    Image out;
    if (setjmp(jerr.setjmp_buffer)) {   // any fatal libjpeg error lands here
        jpeg_destroy_decompress(&cinfo);
        return std::nullopt;
    }
    jpeg_CreateDecompress(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
    jpeg_mem_src(&cinfo, d, static_cast<unsigned long>(n));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return std::nullopt;
    }
    cinfo.out_color_space = JCS_EXT_RGBA;   // libjpeg-turbo writes RGBA directly
    if (!jpeg_start_decompress(&cinfo)) {
        jpeg_destroy_decompress(&cinfo);
        return std::nullopt;
    }
    const uint32_t w = cinfo.output_width, h = cinfo.output_height;
    const int comps = cinfo.output_components;   // 4 for JCS_EXT_RGBA
    if (w == 0 || h == 0 || comps <= 0) {
        jpeg_destroy_decompress(&cinfo);
        return std::nullopt;
    }
    pixels.resize(static_cast<size_t>(w) * h * comps);
    const size_t stride = static_cast<size_t>(w) * comps;
    while (cinfo.output_scanline < h) {
        uint8_t* row = pixels.data() + static_cast<size_t>(cinfo.output_scanline) * stride;
        JSAMPROW rows[1] = {row};
        if (jpeg_read_scanlines(&cinfo, rows, 1) != 1) break;
    }
    bool complete = cinfo.output_scanline >= h;
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    if (!complete) return std::nullopt;

    out.width = w; out.height = h;
    if (comps == 4) out.rgba = std::move(pixels);
    else to_rgba(pixels.data(), comps, static_cast<size_t>(w) * h, out.rgba);
    out.hasAlpha = false;                 // baseline JPEG has no alpha
    out.format = "JPEG";
    out.decoder = "libjpeg-turbo " LIBJPEG_TURBO_VERSION;
    return out;
}

// --- libwebp ----------------------------------------------------------------
std::optional<Image> decode_webp(const uint8_t* d, size_t n) {
    WebPBitstreamFeatures f{};
    if (WebPGetFeatures(d, n, &f) != VP8_STATUS_OK) return std::nullopt;
    if (f.width <= 0 || f.height <= 0) return std::nullopt;

    Image out;
    out.width = static_cast<uint32_t>(f.width);
    out.height = static_cast<uint32_t>(f.height);
    out.rgba.resize(static_cast<size_t>(f.width) * f.height * 4);
    if (!WebPDecodeRGBAInto(d, n, out.rgba.data(), out.rgba.size(),
                            static_cast<int>(out.width) * 4)) {
        return std::nullopt;
    }
    out.hasAlpha = f.has_alpha != 0;
    out.format = std::string("WebP (") + (f.format == 2 ? "lossless" : "lossy") +
                 (f.has_animation ? ", animated" : "") + ")";
    out.decoder = "libwebp 1.6.0";
    return out;
}

// --- WIC (OS decoder) --------------------------------------------------------
std::optional<Image> decode_wic(const uint8_t* d, size_t n, const char* label) {
    auto w = castlemist::wic::decode(d, n);
    if (!w) return std::nullopt;
    Image out;
    out.width = w->width; out.height = w->height;
    out.rgba = std::move(w->rgba);
    out.hasAlpha = true;                  // WIC always converts to 32bpp RGBA
    out.format = label;
    out.decoder = "Windows Imaging Component";
    return out;
}

// --- stb_image (fallback) ----------------------------------------------------
std::optional<Image> decode_stb(const uint8_t* d, size_t n, const char* label) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(d, static_cast<int>(n), &w, &h, &comp, 4);
    if (!px) return std::nullopt;
    Image out;
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    out.hasAlpha = (comp == 2 || comp == 4);
    out.format = label;
    out.decoder = "stb_image (fallback)";
    return out;
}

} // namespace

bool is_png(const uint8_t* d, size_t n) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return n >= 8 && std::memcmp(d, sig, 8) == 0;
}
bool is_jpeg(const uint8_t* d, size_t n) {
    return n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
}
bool is_bmp(const uint8_t* d, size_t n) { return n >= 2 && d[0] == 'B' && d[1] == 'M'; }
bool is_gif(const uint8_t* d, size_t n) { return n >= 6 && std::memcmp(d, "GIF8", 4) == 0; }
bool is_webp(const uint8_t* d, size_t n) {
    return n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0;
}

bool is_image(const uint8_t* d, size_t n) {
    if (!d || n < 8) return false;
    return is_png(d, n) || is_jpeg(d, n) || is_webp(d, n) || is_bmp(d, n) || is_gif(d, n);
}

std::optional<Image> decode(const uint8_t* d, size_t n) {
    if (!d || n < 8) return std::nullopt;

    // WebP: only libwebp can do this (WIC needs an optional OS codec pack, stb
    // has no WebP support at all), so there's nothing to fall back to.
    if (is_webp(d, n)) return decode_webp(d, n);

    // JPEG: the OG library first, then the OS, then stb.
    if (is_jpeg(d, n)) {
        if (auto r = decode_jpeg_turbo(d, n)) return r;
        if (auto r = decode_wic(d, n, "JPEG")) return r;
        return decode_stb(d, n, "JPEG");
    }

    // PNG: libpng would head this chain once zlib is available (see header note).
    if (is_png(d, n)) {
        if (auto r = decode_wic(d, n, "PNG")) return r;
        return decode_stb(d, n, "PNG");
    }

    const char* label = is_bmp(d, n) ? "BMP" : (is_gif(d, n) ? "GIF" : "image");
    if (auto r = decode_wic(d, n, label)) return r;
    return decode_stb(d, n, label);
}

std::string decoder_versions() {
    return std::string("JPEG: libjpeg-turbo ") + LIBJPEG_TURBO_VERSION +
           "  |  WebP: libwebp 1.6.0  |  PNG/BMP/GIF: WIC  |  fallback: stb_image";
}

} // namespace castlemist::img
