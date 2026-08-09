#ifndef GW2_STRUCT_TEMPLATE_H
#define GW2_STRUCT_TEMPLATE_H

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

/// Global registry for the packfile struct template (gw2_packfile.json) that
/// castlemist::model::Extractor needs to walk .modl / AMAT chunks. The template is loaded
/// once (auto-detected at startup, or picked via "File -> Load Struct JSON..."),
/// stored behind a mutex, and handed to background extraction threads as a
/// shared_ptr snapshot so a reload never invalidates an in-flight read.
namespace castlemist::tpl {

/// Returns the current template (nullptr if none loaded yet). Never triggers
/// a load -- use this where the caller only wants to know what's already
/// resident (e.g. status/info text), not to pay for a parse.
std::shared_ptr<const nlohmann::json> get();

/// Returns the current template, auto-loading it from the conventional
/// on-disk locations on first call if nothing is loaded yet (see auto_load()).
/// This is the lazy entry point: the multi-megabyte gw2_packfile.json is only
/// ever parsed the first time something actually needs it -- opening the
/// Structure tab, or extracting an entry that turns out to be a model/map --
/// instead of unconditionally at startup. Safe to call from a worker thread;
/// the load itself is still serialized by the same mutex as load_from_file().
std::shared_ptr<const nlohmann::json> get_or_auto_load();

/// Where the active template was loaded from, or empty if none is loaded. The
/// index builder needs the path rather than the parsed JSON, because it runs the
/// parse itself on a worker thread.
std::string source_path();

/// Parses `path` and installs it as the active template. Returns false (leaving
/// the previous template untouched) on read/parse failure; `error` gets why.
bool load_from_file(const std::string& path, std::string& error);

/// Tries the conventional locations relative to the running exe and the working
/// directory (`dumps/packfile/gw2_packfile.json` and friends). Returns true if
/// one loaded. Safe to call once at startup; the application works without a
/// template, it just cannot name packfile fields.
bool auto_load();

} // namespace castlemist::tpl

#endif // GW2_STRUCT_TEMPLATE_H
