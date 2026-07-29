// audio_player.cpp -- MP3 decode (dr_mp3) + waveOut playback. This is the single
// translation unit that instantiates dr_mp3.
#include "castlemist/media/audio_player.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

// Ogg Vorbis decoder (some GW2 sounds are Vorbis, e.g. AMSP music banks). stb_vorbis
// is compiled as a separate C unit (src/stb_vorbis_impl.c); declare its entry point.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate, short** output);

#include "castlemist/native/gw2_audio.hpp" // codec detection (shared with the parser)

#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <vector>

namespace castlemist::snd {
namespace {

HWAVEOUT           g_hwo = nullptr;
WAVEHDR            g_hdr{};
std::vector<int16_t> g_pcm;      // decoded PCM, kept alive across playback + seeks
std::atomic<bool>  g_done{true}; // set by the waveOut callback at WOM_DONE
// Format of the clip currently held in g_pcm (survives stop() so a seek bar can
// still scrub / report duration after playback ends).
unsigned           g_channels = 0;
unsigned           g_sampleRate = 0;
size_t             g_totalFrames = 0;  // g_pcm.size() / g_channels
size_t             g_baseFrame = 0;    // frame offset the current waveOut buffer starts at

// waveOut calls this on its own thread; keep it to a flag set (no waveOut* calls here).
void CALLBACK wave_cb(HWAVEOUT, UINT msg, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
    if (msg == WOM_DONE) g_done.store(true);
}

void close_device() {
    if (!g_hwo) return;
    waveOutReset(g_hwo); // stop + return buffers
    if (g_hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
    waveOutClose(g_hwo);
    g_hwo = nullptr;
    g_hdr = WAVEHDR{};
}

// Decode an MP3 buffer fully to interleaved s16 PCM. Returns false on failure.
bool decode_mp3(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                unsigned& channels, unsigned& sampleRate, double& seconds) {
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, data, size, nullptr)) return false;
    channels = mp3.channels;
    sampleRate = mp3.sampleRate;
    drmp3_uint64 total = drmp3_get_pcm_frame_count(&mp3);
    if (channels == 0 || sampleRate == 0 || total == 0) { drmp3_uninit(&mp3); return false; }
    pcm.assign((size_t)total * channels, 0);
    drmp3_uint64 got = drmp3_read_pcm_frames_s16(&mp3, total, pcm.data());
    pcm.resize((size_t)got * channels);
    seconds = (double)got / sampleRate;
    drmp3_uninit(&mp3);
    return got > 0;
}

// Decode an Ogg Vorbis buffer fully to interleaved s16 PCM (stb_vorbis).
bool decode_ogg(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                unsigned& channels, unsigned& sampleRate, double& seconds) {
    int ch = 0, sr = 0; short* out = nullptr;
    int frames = stb_vorbis_decode_memory(data, (int)size, &ch, &sr, &out);
    if (frames <= 0 || !out || ch <= 0 || sr <= 0) { if (out) free(out); return false; }
    channels = (unsigned)ch; sampleRate = (unsigned)sr;
    pcm.assign(out, out + (size_t)frames * ch);
    free(out);
    seconds = (double)frames / sr;
    return true;
}

// Decode any supported clip (MP3 or Ogg) to interleaved s16 PCM.
bool decode_clip(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                 unsigned& channels, unsigned& sampleRate, double& seconds) {
    auto clips = castlemist::audio::extract(data, size);
    if (clips.empty()) return false;
    const auto& c = clips.front();
    if (c.codec == castlemist::audio::CODEC_MP3) return decode_mp3(c.data.data(), c.data.size(), pcm, channels, sampleRate, seconds);
    if (c.codec == castlemist::audio::CODEC_OGG) return decode_ogg(c.data.data(), c.data.size(), pcm, channels, sampleRate, seconds);
    return false;
}

} // namespace

ClipInfo probe(const uint8_t* data, size_t size) {
    ClipInfo info;
    if (!data || size == 0) return info;
    auto clips = castlemist::audio::extract(data, size);
    if (!clips.empty()) info.codec = castlemist::audio::codecName(clips.front().codec);
    std::vector<int16_t> pcm;
    unsigned ch = 0, sr = 0; double secs = 0;
    if (decode_clip(data, size, pcm, ch, sr, secs)) {
        info.ok = true; info.channels = ch; info.sampleRate = sr; info.seconds = secs;
    }
    return info;
}

// Open the device and start streaming the already-decoded g_pcm from a given
// frame offset (used by both play() and seek()). Leaves g_pcm/format intact.
static bool start_at(size_t frameOffset) {
    close_device();
    if (g_pcm.empty() || g_channels == 0 || g_sampleRate == 0) return false;
    if (frameOffset >= g_totalFrames) frameOffset = 0;
    g_baseFrame = frameOffset;

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)g_channels;
    wf.nSamplesPerSec = g_sampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, (DWORD_PTR)wave_cb, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        g_hwo = nullptr;
        return false;
    }
    size_t sampleOff = frameOffset * g_channels;
    g_hdr = WAVEHDR{};
    g_hdr.lpData = (LPSTR)(g_pcm.data() + sampleOff);
    g_hdr.dwBufferLength = (DWORD)((g_pcm.size() - sampleOff) * sizeof(int16_t));
    if (waveOutPrepareHeader(g_hwo, &g_hdr, sizeof(g_hdr)) != MMSYSERR_NOERROR) { close_device(); return false; }
    g_done.store(false);
    if (waveOutWrite(g_hwo, &g_hdr, sizeof(g_hdr)) != MMSYSERR_NOERROR) { g_done.store(true); close_device(); return false; }
    return true;
}

bool play(const uint8_t* data, size_t size) {
    stop();
    g_pcm.clear();
    g_channels = g_sampleRate = 0; g_totalFrames = 0; g_baseFrame = 0;
    if (!data || size == 0) return false;

    double seconds = 0;
    if (!decode_clip(data, size, g_pcm, g_channels, g_sampleRate, seconds)) { g_pcm.clear(); return false; }
    g_totalFrames = g_channels ? g_pcm.size() / g_channels : 0;
    return start_at(0);
}

double duration_seconds() {
    return g_sampleRate ? (double)g_totalFrames / g_sampleRate : 0.0;
}

double position_seconds() {
    if (!g_hwo || g_sampleRate == 0) return 0.0;
    MMTIME mt{}; mt.wType = TIME_SAMPLES;
    double base = (double)g_baseFrame / g_sampleRate;
    if (waveOutGetPosition(g_hwo, &mt, sizeof mt) != MMSYSERR_NOERROR || mt.wType != TIME_SAMPLES)
        return base;
    double s = base + (double)mt.u.sample / g_sampleRate;
    double dur = duration_seconds();
    return (dur > 0 && s > dur) ? dur : s;
}

bool seek(double seconds) {
    if (g_pcm.empty() || g_sampleRate == 0) return false;
    if (seconds < 0) seconds = 0;
    size_t frame = (size_t)(seconds * g_sampleRate);
    if (frame >= g_totalFrames && g_totalFrames > 0) frame = g_totalFrames - 1;
    return start_at(frame);
}

void stop() {
    close_device();
    g_done.store(true);
}

bool is_playing() {
    return g_hwo != nullptr && !g_done.load();
}

void shutdown() { stop(); }

} // namespace castlemist::snd
