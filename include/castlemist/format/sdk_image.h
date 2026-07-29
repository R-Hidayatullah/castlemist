#ifndef GW2_SDK_IMAGE_H
#define GW2_SDK_IMAGE_H

// Still-image decoding for the browser's preview surface, using the ORIGINAL
// reference libraries first and falling back only when they can't handle a buffer:
//
//   JPEG      libjpeg-turbo 3.2.0  ->  WIC  ->  stb_image
//   WebP      libwebp 1.6.0        ->  (no fallback: stb has no WebP decoder)
//   PNG       WIC                  ->  stb_image        [see note]
//   BMP/GIF/TIFF/TGA/PSD  WIC      ->  stb_image
//
// Note: libpng 1.6.58 ships in the SDK folder but needs zlib, which isn't present
// on this toolchain, so PNG currently uses WIC (the OS decoder) then stb. Drop a
// zlib in and libpng can be added to the front of the PNG chain the same way.
//
// Libraries are built once into third_party/lib/{libjpegturbo,libwebpdec}.a --
// see the build recipe note in the castlemist memory entry.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace castlemist::img {

/// @brief A decoded still image, normalized to RGBA8888, plus the decoder that produced it.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;   // tightly packed, pitch == width*4, RGBA8888 order
    bool hasAlpha = false;       // source carried a real alpha channel
    std::string format;          // source format, e.g. "JPEG", "WebP (lossy)", "PNG"
    std::string decoder;         // which library produced it, e.g. "libjpeg-turbo 3.2.0"
};

/// --- format sniffing (magic only; safe on truncated buffers) -----------------
bool is_png(const uint8_t* d, size_t n);
bool is_jpeg(const uint8_t* d, size_t n);
bool is_bmp(const uint8_t* d, size_t n);
bool is_gif(const uint8_t* d, size_t n);
/// RIFF....WEBP -- GW2 stores a number of UI images this way.
bool is_webp(const uint8_t* d, size_t n);

/// True for any still-image container this module can attempt.
bool is_image(const uint8_t* d, size_t n);

/// Decode to RGBA8888 using the priority chain above. std::nullopt when every
/// applicable decoder declined the buffer.
std::optional<Image> decode(const uint8_t* d, size_t n);

/// Human summary of the compiled-in decoders (shown in the info panel / about).
std::string decoder_versions();

} // namespace castlemist::img

#endif // GW2_SDK_IMAGE_H
