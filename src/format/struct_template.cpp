#include "castlemist/format/struct_template.h"

#include <fstream>
#include <mutex>
#include <vector>

#include <windows.h>

namespace castlemist::tpl {

namespace {

std::mutex g_mutex;
std::shared_ptr<const nlohmann::json> g_template;
std::string g_source_path;
// Set once auto_load() has been attempted (success or failure) so
// get_or_auto_load() only ever tries the disk scan once per process, instead
// of re-stat'ing every candidate path on every entry click when no template
// exists to find.
bool g_auto_load_attempted = false;

// Directory the running .exe lives in (with trailing backslash), for locating a
// bundled templates/ folder regardless of the process working directory.
std::string exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, n);
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool file_exists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

std::shared_ptr<const nlohmann::json> get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_template;
}

std::string source_path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_source_path;
}

bool load_from_file(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open " + path;
        return false;
    }
    try {
        auto parsed = std::make_shared<nlohmann::json>();
        in >> *parsed;
        if (!parsed->contains("types")) {
            error = "Template has no 'types' section: " + path;
            return false;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_template = parsed;
        g_source_path = path;
        g_auto_load_attempted = true; // an explicit/successful load also satisfies auto_load's job
        return true;
    } catch (const std::exception& e) {
        error = std::string("Parse error: ") + e.what();
        return false;
    }
}

bool auto_load() {
    // The template is generated output, so it lives in dumps/packfile/ rather
    // than in the source tree -- see tools/structs. The exe runs from
    // build/<preset>/bin/, so climbing three levels reaches the repository root;
    // the working-directory entries cover being run from the root itself.
    const std::string dir = exe_dir();
    const std::vector<std::string> candidates = {
        dir + "gw2_packfile.json",
        dir + "..\\..\\..\\dumps\\packfile\\gw2_packfile.json",
        dir + "..\\..\\dumps\\packfile\\gw2_packfile.json",
        dir + "..\\dumps\\packfile\\gw2_packfile.json",
        "dumps\\packfile\\gw2_packfile.json",
        "..\\dumps\\packfile\\gw2_packfile.json",
        "gw2_packfile.json",
    };
    std::string error;
    for (const std::string& path : candidates) {
        if (file_exists(path) && load_from_file(path, error)) {
            return true;
        }
    }
    // Nothing found: remember that we tried, so get_or_auto_load() doesn't
    // re-stat every candidate path again on the next entry click.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_auto_load_attempted = true;
    return false;
}

std::shared_ptr<const nlohmann::json> get_or_auto_load() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_template) {
            return g_template;
        }
        if (g_auto_load_attempted) {
            // Already looked for it once (found nothing) and no explicit
            // File -> Load Struct JSON... has happened since -- don't keep
            // re-parsing/re-stat'ing on every click of an entry or tab.
            return nullptr;
        }
    }
    // First real need for the template in this process: do the (possibly
    // multi-megabyte) parse now, on whichever thread asked for it, instead of
    // blocking window creation before any file was ever opened.
    auto_load();
    return get();
}

} // namespace castlemist::tpl
