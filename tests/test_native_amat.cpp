/// @file
/// @brief AMAT effect-selection primitives: token decoding and quality ranking.
///
/// These are the rules the engine itself uses to decide which shader in an AMAT
/// draws a material (`sub_140BFAD20` / `sub_140BFBFD0`, BgfxShader.cpp). They are
/// pure functions over values that are documented and reproducible, so unlike the
/// end-to-end model tests they need no Gw2.dat and never skip.

#include "test_framework.h"

#include "castlemist/native/gw2model.hpp"

using castlemist::model::amatQualityRank;
using castlemist::model::amatRemapEffectToken;
using castlemist::model::decodeToken23;
using castlemist::model::kAmatDefaultEffectToken;

// GW2 packs short lowercase names into a token32 base-23 (no j/q/z) with a
// 0x30000000 bias -- `Token::Decode`, sub_140E3B360. These three are read
// straight out of AMAT 19896's techniques[].quality, so they pin the decoder to
// real archive data rather than to a re-derivation of the same arithmetic.
CM_TEST(amat, decodes_real_quality_tokens_to_their_names) {
    CHECK(decodeToken23(805394902u) == "high");
    CHECK(decodeToken23(881522146u) == "medium");
    CHECK(decodeToken23(805317257u) == "low");
}

// techniques[] are quality LEVELS, not passes. A viewer has no graphics-settings
// slider, so selection takes the best tier the AMAT offers -- which only works if
// the ranking is strictly ordered.
CM_TEST(amat, ranks_quality_tiers_in_order) {
    const int low = amatQualityRank(805317257u);
    const int medium = amatQualityRank(881522146u);
    const int high = amatQualityRank(805394902u);

    CHECK(low < medium);
    CHECK(medium < high);

    // An unrecognised token must stay usable but never outrank a real tier,
    // otherwise one odd AMAT would drag selection down to its worst shader.
    CHECK(amatQualityRank(0u) < low);
}

// A token the pass does not carry walks a hardcoded remap chain and then lands on
// the default. Every chain has to terminate: `extractAmat` bounds the walk, but a
// remap that returned its own input would still burn the whole budget.
CM_TEST(amat, effect_token_remap_chain_terminates_at_the_default) {
    CHECK_EQ(kAmatDefaultEffectToken, 183330ull);

    // An arbitrary token is not in the chain -> straight to the default.
    CHECK_EQ(amatRemapEffectToken(0x0B58860Full), kAmatDefaultEffectToken);

    // The three known chain entries each hop somewhere else first, and that hop
    // must itself resolve to the default rather than loop.
    for (uint64_t start : {0x1481311ECull, 0x1779028991ECull, 0x2C26C0B64F711ECull}) {
        const uint64_t next = amatRemapEffectToken(start);
        CHECK(next != start);
        CHECK(next != kAmatDefaultEffectToken);          // it really is a hop
        CHECK_EQ(amatRemapEffectToken(next), kAmatDefaultEffectToken);
    }

    // The default maps to itself, which is what stops the walk.
    CHECK_EQ(amatRemapEffectToken(kAmatDefaultEffectToken), kAmatDefaultEffectToken);
}
