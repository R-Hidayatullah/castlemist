/// @file
/// @brief Tests for the audio player's and video player's probe/reject paths.
///
/// Playback itself needs a sound device and a real clip, so what is checked
/// here is the half that runs on every entry the user clicks: deciding whether
/// a buffer is playable at all, and staying inert when it is not.

#include "test_framework.h"

#include "castlemist/media/audio_player.h"
#include "castlemist/media/video_player.h"

#include <vector>

CM_TEST(audio, probing_garbage_reports_failure_rather_than_guessing) {
    std::vector<uint8_t> junk(4096, 0x5A);
    castlemist::snd::ClipInfo info = castlemist::snd::probe(junk.data(), junk.size());
    CHECK_FALSE(info.ok);
    CHECK_EQ(info.channels, 0u);
    CHECK_EQ(info.sampleRate, 0u);
}

CM_TEST(audio, probing_an_empty_buffer_is_safe) {
    castlemist::snd::ClipInfo info = castlemist::snd::probe(nullptr, 0);
    CHECK_FALSE(info.ok);

    std::vector<uint8_t> empty;
    CHECK_FALSE(castlemist::snd::probe(empty.data(), empty.size()).ok);
}

// An MP3 frame header alone is not a decodable clip; accepting it would leave
// the transport showing a duration for something that will never play.
CM_TEST(audio, a_bare_frame_sync_is_not_a_playable_clip) {
    std::vector<uint8_t> stub{0xFF, 0xFB, 0x90, 0x00};
    stub.resize(64, 0);
    CHECK_FALSE(castlemist::snd::probe(stub.data(), stub.size()).ok);
}

CM_TEST(audio, playing_garbage_fails_and_leaves_the_player_idle) {
    std::vector<uint8_t> junk(1024, 0x11);
    CHECK_FALSE(castlemist::snd::play(junk.data(), junk.size()));
    CHECK_FALSE(castlemist::snd::is_playing());
    CHECK_NEAR(castlemist::snd::position_seconds(), 0.0, 1e-9);
}

CM_TEST(audio, seeking_with_no_clip_loaded_fails) {
    castlemist::snd::stop();
    CHECK_FALSE(castlemist::snd::seek(1.0));
}

CM_TEST(audio, stop_and_shutdown_are_safe_when_idle) {
    castlemist::snd::stop();
    castlemist::snd::stop();
    CHECK_FALSE(castlemist::snd::is_playing());
    castlemist::snd::shutdown();
    castlemist::snd::shutdown();
    CHECK_FALSE(castlemist::snd::is_playing());
}

CM_TEST(video, recognises_bink_signatures) {
    for (const char* magic : {"KB2i", "KB2j", "BIKi"}) {
        std::vector<uint8_t> b(magic, magic + 4);
        b.resize(64, 0);
        CHECK(castlemist::vid::is_video(b.data(), b.size()));
    }
}

CM_TEST(video, rejects_non_video_buffers) {
    std::vector<uint8_t> pf{'P', 'F', 0, 0};
    pf.resize(64, 0);
    CHECK_FALSE(castlemist::vid::is_video(pf.data(), pf.size()));

    std::vector<uint8_t> tiny{'K', 'B'};
    CHECK_FALSE(castlemist::vid::is_video(tiny.data(), tiny.size()));
    CHECK_FALSE(castlemist::vid::is_video(nullptr, 0));
}
