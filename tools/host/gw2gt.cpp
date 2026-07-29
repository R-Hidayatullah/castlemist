// gw2gt.cpp -- decode an ATEX-family texture with the GAME'S OWN decoder, by
// calling into Gw2-64.dll (see tools/mkgw2dll.py). Ground truth for the
// ArenaNet container layer: the custom header + per-mip RLE/constant-fill
// "inflate" that sits in front of otherwise standard BC1..BC7 blocks.
//
// The interesting property of this tool is that it boots NOTHING. No WinMain,
// no window, no D3D, no login, no Gw2.dat. The image pipeline turns out to be
// reachable cold, because everything it needs is either static const data
// (format tables) or routed through a handful of engine calls we can stand in
// for:
//
//   Arena\Engine\Gr\Img\ImgDecode.cpp     the streaming decoder we drive
//     ImgDecodeInit  0x140B15850          zeroes the context
//     ImgDecodeStep  0x140B158E0          sniffs magic, dispatches per filetype
//     ImgDecodeEnd   0x140B15740          teardown
//   Arena\Engine\Gr\Img\ImgMem.cpp        the destination image
//     ImgMemAllocCube 0x140B3E470         (format, dims, levels, pool)
//
// Only the allocator needs standing in for. ExeMemFlexible's ExeMemAlloc
// (0x1409CCE30) forwards to function pointers that main() installs, and its
// stats call walks a category table that is null until then -- so it is
// detoured to plain malloc. Nothing else in the decode path touches engine
// state, which is why this works without the client running.
//
// Usage: gw2gt <in.atex> <out.png> [--mip N] [--dll <path>] [--dump <blocks.bin>]

#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "gw2_atex.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

// --------------------------------------------------------------- addresses --
// Every RVA below is an IDA address from the 0x140000000-based database. The
// converted DLL keeps its preferred base, so these normally resolve 1:1; the
// slide is applied anyway so a relocated load still works.

constexpr uintptr_t kIdaBase = 0x140000000ull;
uintptr_t g_base = 0;

template <class T> T at(uintptr_t ida_addr) {
    return reinterpret_cast<T>(g_base + (ida_addr - kIdaBase));
}

// Arena\Engine\Gr\Img
using pImgDecodeInit    = void*    (*)(void*);
using pImgDecodeStep    = int      (*)(void*);
using pImgDecodeEnd     = void*    (*)(void*);
using pImgMemAllocCube  = void**   (*)(uint32_t fmt, const uint32_t* dims,
                                       uint32_t levels, uint16_t pool);
using pImgFmtBitCount   = uint32_t (*)(uint32_t fmt);
using pImgFmtBlockDims  = void*    (*)(void* out, uint32_t fmt);
using pImgCalcLevelSize = uint32_t (*)(uint32_t bits, const void* blockDims,
                                       const void* dims, uint32_t level);

pImgDecodeInit    ImgDecodeInit;
pImgDecodeStep    ImgDecodeStep;
pImgDecodeEnd     ImgDecodeEnd;
pImgMemAllocCube  ImgMemAllocCube;
pImgFmtBitCount   ImgFmtBitCount;
pImgFmtBlockDims  ImgFmtBlockDims;
pImgCalcLevelSize ImgCalcLevelSize;

// ImgDecoder field offsets, read out of ImgDecodeInit/Step and GrImage.cpp's
// use of them (sub_140A7ADD0).
enum : size_t {
    DEC_IN        = 0,    // const void*  input cursor
    DEC_IN_AVAIL  = 8,    // u32          bytes readable at DEC_IN
    DEC_FILE_SIZE = 12,   // u32          total file size
    DEC_PLATFORM  = 16,   // u32          1..6; anything but 5/6 means little-endian
    DEC_IMAGE     = 40,   // void*        destination, supplied by us on demand
    DEC_DIMS      = 56,   // u32[2]       width, height
    DEC_FORMAT    = 68,   // u32          GR_FORMAT (26 == DXT5)
    DEC_LEVELS    = 72,   // u32          mip count, filled in at the end
    // Hex-Rays reads GrImage.cpp's cross-check as decoder+52, but on a live
    // decode +52 stays 0 while +76 -- which ImgDecodeInit explicitly clears --
    // holds 2 at exactly the step that asks for an image. Trust the observation.
    DEC_STATUS    = 76,   // u32          2 == "give me an image"
    DEC_ERROR     = 80,   // const char*  set when a step reports corruption
};

// ImgDecodeStep return codes, named after how GrImage.cpp branches on them.
enum {
    STEP_EOF      = 0,
    STEP_DONE     = 1,
    STEP_NEED_IMG = 2,
    STEP_CONTINUE = 3,
    STEP_CORRUPT  = 4,
};

bool g_verbose = false;

void vlog(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// ------------------------------------------------------- allocator stand-in --
//
// ExeMemAlloc's real body dispatches through qword_142680B50/B70/B48 and then
// reports into a memory-category table, all of which main() populates. Cold,
// those are null, so the whole function is replaced rather than patched. The
// tool is one-shot, so frees are deliberately dropped.

size_t g_alloc_bytes = 0;
int    g_alloc_count = 0;

void* WINAPI h_ExeMemAlloc(uint8_t tag, uint64_t bytes, uint64_t /*a3*/,
                           int zero_fill, uint64_t /*name*/, int /*a6*/,
                           uint16_t /*pool*/) {
    void* p = _aligned_malloc((size_t)bytes, 16);
    if (p) {
        // The engine only zeroes on request, but a cold heap has no poison
        // pattern of its own, so always clear: it makes a decoder that reads
        // an uninitialised field fail the same way every run.
        memset(p, 0, (size_t)bytes);
        ++g_alloc_count;
        g_alloc_bytes += (size_t)bytes;
    }
    vlog("[mem] alloc tag=%u bytes=%llu -> %p\n", tag, (unsigned long long)bytes, p);
    return p;
}

// ExeMemUsableSize; only feeds a statistics counter here.
uint64_t WINAPI h_ExeMemUsableSize(void* /*p*/) { return 0; }

void WINAPI h_ExeMemFree(void* /*p*/, uint64_t, uint64_t) {}

// --------------------------------------------------------- diagnostic traps --
//
// The client's assert reporter captures context, writes Crash.dmp and exits.
// That is exactly wrong for a CLI, so both reporters just print. Neither is
// expected to fire on a well-formed texture; if one does, the message is the
// most useful thing the tool can say.

int g_asserts = 0;

uint64_t WINAPI h_Assert(const char* expr, const char* file, int line) {
    ++g_asserts;
    fprintf(stderr, "[assert] %s\n          at %s:%d\n",
            expr ? expr : "(null)", file ? file : "(null)", line);
    return 0;
}

uint64_t h_Fatal(const char* file, uint64_t line, const char* fmt, ...) {
    fprintf(stderr, "[fatal] %s:%llu  ", file ? file : "(null)",
            (unsigned long long)line);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt ? fmt : "(null)", ap);
    va_end(ap);
    fputc('\n', stderr);
    return 0;
}

uint64_t h_Log(uint64_t level, const char* fmt, ...) {
    if (g_verbose) {
        fprintf(stderr, "[game:%llu] ", (unsigned long long)level);
        va_list ap; va_start(ap, fmt);
        vfprintf(stderr, fmt ? fmt : "", ap);
        va_end(ap);
        fputc('\n', stderr);
    }
    return 0;
}

bool hook(uintptr_t ida_addr, void* detour, const char* name) {
    void* target = at<void*>(ida_addr);
    MH_STATUS s = MH_CreateHook(target, detour, nullptr);
    if (s != MH_OK) {
        fprintf(stderr, "MH_CreateHook(%s @ %p) failed: %s\n",
                name, target, MH_StatusToString(s));
        return false;
    }
    return true;
}

bool install_stubs() {
    if (MH_Initialize() != MH_OK) { fprintf(stderr, "MH_Initialize failed\n"); return false; }
    bool ok = true;
    ok &= hook(0x1409CCE30, (void*)&h_ExeMemAlloc,     "ExeMemAlloc");
    ok &= hook(0x1409CD1C0, (void*)&h_ExeMemUsableSize,"ExeMemUsableSize");
    ok &= hook(0x1409CC6C0, (void*)&h_ExeMemFree,      "ExeMemFree");
    ok &= hook(0x1409DAA30, (void*)&h_Assert,          "Assert");
    ok &= hook(0x1409DABE0, (void*)&h_Fatal,           "Fatal");
    ok &= hook(0x1409BAB40, (void*)&h_Log,             "Log");
    if (!ok) return false;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { fprintf(stderr, "MH_EnableHook failed\n"); return false; }
    return true;
}

// ------------------------------------------------------------------- driver --

struct Decoded {
    uint32_t format = 0, levels = 0, width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levelData;   // face 0, level 0..levels-1
};

bool run_decoder(const std::vector<uint8_t>& file, Decoded& out) {
    // The context is 152 bytes of live fields; over-allocate and zero so any
    // field this port has not accounted for reads as 0 rather than stack junk.
    alignas(16) uint8_t dec[512] = {};

    *(const void**)(dec + DEC_IN)      = file.data();
    *(uint32_t*)   (dec + DEC_IN_AVAIL)= (uint32_t)file.size();
    *(uint32_t*)   (dec + DEC_FILE_SIZE)= (uint32_t)file.size();
    *(uint32_t*)   (dec + DEC_PLATFORM)= 1;          // little-endian PC

    ImgDecodeInit(dec);
    // ImgDecodeInit clobbers the input fields' neighbours but not these three,
    // so they are set before the call the way GrImage.cpp does it.
    *(const void**)(dec + DEC_IN)       = file.data();
    *(uint32_t*)   (dec + DEC_IN_AVAIL) = (uint32_t)file.size();
    *(uint32_t*)   (dec + DEC_FILE_SIZE)= (uint32_t)file.size();
    *(uint32_t*)   (dec + DEC_PLATFORM) = 1;

    void** image = nullptr;
    for (int guard = 0; guard < 1024; ++guard) {
        int r = ImgDecodeStep(dec);
        uint32_t status = *(uint32_t*)(dec + DEC_STATUS);
        vlog("[step] r=%d status=%u fmt=%u dims=%ux%u levels=%u\n", r, status,
             *(uint32_t*)(dec + DEC_FORMAT),
             *(uint32_t*)(dec + DEC_DIMS), *(uint32_t*)(dec + DEC_DIMS + 4),
             *(uint32_t*)(dec + DEC_LEVELS));

        if (r == STEP_DONE) break;

        if (r == STEP_NEED_IMG) {
            // GrImage.cpp cross-checks decoder+52 == 2 here. Treat a mismatch as
            // information rather than a stop: the fields that actually matter
            // (format, dims) are validated below.
            if (status != STEP_NEED_IMG)
                vlog("[step] note: status field is %u, GrImage expects 2\n", status);
            if (g_verbose) {
                for (int off = 0; off < 160; off += 16) {
                    vlog("  ctx+%03d:", off);
                    for (int k = 0; k < 16; ++k) vlog(" %02x", dec[off + k]);
                    vlog("\n");
                }
            }
            uint32_t fmt = *(uint32_t*)(dec + DEC_FORMAT);
            image = ImgMemAllocCube(fmt, (const uint32_t*)(dec + DEC_DIMS), 0, 0);
            if (!image) { fprintf(stderr, "ImgMemAllocCube returned null\n"); ImgDecodeEnd(dec); return false; }
            *(void**)(dec + DEC_IMAGE) = image;
            continue;
        }
        if (r == STEP_CONTINUE) continue;

        const char* msg = *(const char**)(dec + DEC_ERROR);
        fprintf(stderr, r == STEP_CORRUPT ? "image is corrupt: %s\n"
                                          : "unexpected end of image (%s)\n",
                msg ? msg : "no detail");
        ImgDecodeEnd(dec);
        return false;
    }

    out.format = *(uint32_t*)(dec + DEC_FORMAT);
    out.levels = *(uint32_t*)(dec + DEC_LEVELS);
    out.width  = *(uint32_t*)(dec + DEC_DIMS);
    out.height = *(uint32_t*)(dec + DEC_DIMS + 4);

    if (!image) { fprintf(stderr, "decoder finished without ever asking for an image\n"); ImgDecodeEnd(dec); return false; }

    // ImgMemAllocCube lays out six faces; image[f] points at that face's array
    // of per-level pointers (ImgMem.cpp sub_140B3DA40). 2D textures use face 0.
    uint8_t  blockDims[24] = {};
    ImgFmtBlockDims(blockDims, out.format);
    uint32_t bits = ImgFmtBitCount(out.format);
    uint32_t dims[2] = { out.width, out.height };

    void** face0 = (void**)image[0];
    for (uint32_t lv = 0; lv < out.levels; ++lv) {
        uint32_t n = ImgCalcLevelSize(bits, blockDims, dims, lv);
        const uint8_t* p = (const uint8_t*)face0[lv];
        out.levelData.emplace_back(p, p + n);
    }

    ImgDecodeEnd(dec);
    return true;
}

// ------------------------------------------------------------------- files --

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void usage() {
    printf("gw2gt -- decode an ATEX texture with the game's own decoder\n"
           "\n"
           "  gw2gt <in.atex> <out.png> [options]\n"
           "\n"
           "  --mip N        mip level to write as PNG (default 0)\n"
           "  --dll <path>   Gw2-64.dll produced by tools/mkgw2dll.py\n"
           "  --dump <file>  write the game's raw level-0 blocks\n"
           "  --compare      also run this repo's gw2_atex.hpp and diff the blocks\n"
           "  -v             trace decoder steps and allocations\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string in_path, out_path, dll_path, dump_path;
    int  mip = 0;
    bool compare = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--mip")     mip = atoi(next("--mip").c_str());
        else if (a == "--dll")     dll_path = next("--dll");
        else if (a == "--dump")    dump_path = next("--dump");
        else if (a == "--compare") compare = true;
        else if (a == "-v")        g_verbose = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (in_path.empty())  in_path = a;
        else if (out_path.empty()) out_path = a;
        else { fprintf(stderr, "unexpected argument: %s\n", a.c_str()); return 2; }
    }
    if (in_path.empty() || out_path.empty()) { usage(); return 2; }

    if (dll_path.empty()) {
        dll_path = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
                   "Guild Wars 2\\Gw2-64.dll";
    }

    std::vector<uint8_t> file;
    if (!read_file(in_path.c_str(), file)) {
        fprintf(stderr, "cannot read %s\n", in_path.c_str());
        return 1;
    }
    printf("input      %s (%zu bytes, magic %.4s)\n", in_path.c_str(), file.size(),
           file.size() >= 4 ? (const char*)file.data() : "????");

    HMODULE gw2 = LoadLibraryExW(widen(dll_path).c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!gw2) {
        fprintf(stderr, "LoadLibraryEx(%s) failed: %lu\n", dll_path.c_str(), GetLastError());
        return 1;
    }
    g_base = (uintptr_t)gw2;
    printf("client     %s @ %p%s\n", dll_path.c_str(), (void*)g_base,
           g_base == kIdaBase ? " (preferred base)" : " (relocated)");

    ImgDecodeInit    = at<pImgDecodeInit>   (0x140B15850);
    ImgDecodeStep    = at<pImgDecodeStep>   (0x140B158E0);
    ImgDecodeEnd     = at<pImgDecodeEnd>    (0x140B15740);
    ImgMemAllocCube  = at<pImgMemAllocCube> (0x140B3E470);
    ImgFmtBitCount   = at<pImgFmtBitCount>  (0x140B3D840);
    ImgFmtBlockDims  = at<pImgFmtBlockDims> (0x140B3D990);
    ImgCalcLevelSize = at<pImgCalcLevelSize>(0x140B3C830);

    if (!install_stubs()) return 1;

    Decoded d;
    if (!run_decoder(file, d)) return 1;

    printf("game says  format=%u (%s) %ux%u, %u mip%s, %d alloc%s / %zu bytes",
           d.format, gw2atex::format_name((int)d.format), d.width, d.height,
           d.levels, d.levels == 1 ? "" : "s",
           g_alloc_count, g_alloc_count == 1 ? "" : "s", g_alloc_bytes);
    if (g_asserts) printf(", %d assert%s", g_asserts, g_asserts == 1 ? "" : "s");
    printf("\n");

    if (mip < 0 || (uint32_t)mip >= d.levels) {
        fprintf(stderr, "mip %d out of range (0..%u)\n", mip, d.levels - 1);
        return 1;
    }

    if (!dump_path.empty()) {
        FILE* f = fopen(dump_path.c_str(), "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", dump_path.c_str()); return 1; }
        fwrite(d.levelData[0].data(), 1, d.levelData[0].size(), f);
        fclose(f);
        printf("dumped     %s (%zu bytes, level 0 blocks)\n",
               dump_path.c_str(), d.levelData[0].size());
    }

    if (compare) {
        try {
            gw2atex::Texture t = gw2atex::parse(file.data(), file.size());
            printf("ours says  format=%s %dx%d, %zu mips\n",
                   t.fmt_name.c_str(), t.width, t.height, t.mips.size());
            size_t n = t.mips.size() < d.levelData.size() ? t.mips.size() : d.levelData.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& a = d.levelData[i];
                const auto& b = t.mips[i].surface;
                if (a.size() != b.size()) {
                    printf("  mip %-2zu SIZE  game %zu vs ours %zu\n", i, a.size(), b.size());
                    continue;
                }
                size_t diff = 0, first = (size_t)-1;
                for (size_t k = 0; k < a.size(); ++k)
                    if (a[k] != b[k]) { if (!diff) first = k; ++diff; }
                if (!diff) printf("  mip %-2zu MATCH %zu bytes\n", i, a.size());
                else       printf("  mip %-2zu DIFF  %zu/%zu bytes, first at %zu\n",
                                  i, diff, a.size(), first);
            }
        } catch (const std::exception& e) {
            printf("ours says  parse failed: %s\n", e.what());
        }
    }

    int w = (int)(d.width  >> mip); if (!w) w = 1;
    int h = (int)(d.height >> mip); if (!h) h = 1;
    gw2atex::Image im = gw2atex::decode_surface((int)d.format, d.levelData[mip].data(),
                                                d.levelData[mip].size(), w, h);
    if (!stbi_write_png(out_path.c_str(), im.width, im.height, 4,
                        im.rgba.data(), im.width * 4)) {
        fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return 1;
    }
    printf("wrote      %s (%dx%d RGBA, mip %d)\n", out_path.c_str(), im.width, im.height, mip);
    return 0;
}
