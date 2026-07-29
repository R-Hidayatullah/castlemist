/// @file
/// @brief Test-runner entry point shared by every castlemist test executable.

#include "test_framework.h"

#include <algorithm>

namespace castlemist::test {

std::vector<TestCase>& registry() {
    // Function-local so registration from other translation units' static
    // initialisers cannot race the vector's own construction.
    static std::vector<TestCase> cases;
    return cases;
}

int run_all(const char* filter) {
    std::vector<TestCase>& cases = registry();
    std::stable_sort(cases.begin(), cases.end(), [](const TestCase& a, const TestCase& b) {
        int c = std::strcmp(a.group, b.group);
        return c != 0 ? c < 0 : std::strcmp(a.name, b.name) < 0;
    });

    int passed = 0, failed = 0, skipped = 0;
    const char* group = nullptr;

    for (const TestCase& t : cases) {
        std::string full = std::string(t.group) + "." + t.name;
        if (filter && *filter && full.find(filter) == std::string::npos) continue;

        if (!group || std::strcmp(group, t.group) != 0) {
            group = t.group;
            std::printf("\n[%s]\n", group);
        }

        try {
            t.fn();
            std::printf("  ok    %s\n", t.name);
            ++passed;
        } catch (const Skipped& s) {
            std::printf("  skip  %s  (%s)\n", t.name, s.reason.c_str());
            ++skipped;
        } catch (const Failure& f) {
            std::printf("  FAIL  %s\n      %s\n", t.name, f.message.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s\n      unexpected exception: %s\n", t.name, e.what());
            ++failed;
        } catch (...) {
            std::printf("  FAIL  %s\n      unexpected non-standard exception\n", t.name);
            ++failed;
        }
    }

    std::printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

} // namespace castlemist::test

int main(int argc, char** argv) {
    return castlemist::test::run_all(argc > 1 ? argv[1] : nullptr);
}
