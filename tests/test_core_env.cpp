/// @file
/// @brief Tests for the typed `GW2_*` environment hook accessors.

#include "test_framework.h"

#include "castlemist/core/env.h"

#include <cstdlib>

using namespace castlemist::core;

namespace {

/// Set (or, with a null value, clear) an environment variable for one test.
struct ScopedEnv {
    std::string name;
    explicit ScopedEnv(const char* n, const char* v) : name(n) {
        _putenv_s(n, v ? v : "");
    }
    ~ScopedEnv() { _putenv_s(name.c_str(), ""); }
};

const char* kVar = "GW2_TEST_ENV_HOOK";

} // namespace

CM_TEST(env, unset_variables_use_the_fallback) {
    CHECK_FALSE(env::is_set(kVar));
    CHECK_FALSE(env::flag(kVar));
    CHECK(env::flag(kVar, true));
    CHECK_EQ(env::integer(kVar, -7), -7);
    CHECK_EQ(env::unsigned_integer(kVar, 9u), 9u);
    CHECK_NEAR(env::real(kVar, 1.5f), 1.5f, 1e-6);
    CHECK(env::text(kVar).empty());
}

CM_TEST(env, presence_is_distinct_from_truth) {
    ScopedEnv e(kVar, "0");
    CHECK(env::is_set(kVar));      // GW2_MATDBG-style hooks test presence
    CHECK_FALSE(env::flag(kVar));  // ...but "0" still means off
}

CM_TEST(env, parses_integers) {
    ScopedEnv e(kVar, "291977");
    CHECK_EQ(env::integer(kVar), 291977);
    CHECK_EQ(env::unsigned_integer(kVar), 291977u);
    CHECK_EQ(env::text(kVar), std::string("291977"));
}

CM_TEST(env, accepts_hex_for_ids) {
    ScopedEnv e(kVar, "0x1F");
    CHECK_EQ(env::integer(kVar), 31);
}

CM_TEST(env, parses_reals) {
    ScopedEnv e(kVar, "2.5");
    CHECK_NEAR(env::real(kVar, 1.0f), 2.5f, 1e-6);
}

// GW2_LIGHT=0 would black out the scene, so values at or below the minimum are
// treated as "not configured" rather than applied.
CM_TEST(env, rejects_values_at_or_below_the_minimum) {
    ScopedEnv e(kVar, "0");
    CHECK_NEAR(env::real(kVar, 1.0f, 0.01f), 1.0f, 1e-6);
}

CM_TEST(env, garbage_values_do_not_crash) {
    ScopedEnv e(kVar, "not-a-number");
    CHECK_EQ(env::integer(kVar, 42), 0);        // strtol yields 0, not the fallback
    CHECK_NEAR(env::real(kVar, 1.0f), 1.0f, 1e-6); // atof yields 0 -> below minimum
}
