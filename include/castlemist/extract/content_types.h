/// @file
/// @brief cntc PackContent objects: the game's item/skill/skin datastore.
/// @ingroup extract

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @addtogroup extract
/// @{

/// @brief One content object inside a cntc `PackContent`.
///
/// Drives the three-pane content browser: types -> entries -> assets.
///
/// @par Where the fields come from
/// The content type is read from `PackContentIndexEntry[+0]`, **not** from
/// `content + 16`. Measured over all 32549 entries of a live cntc, field 0 has
/// 103 clean distinct values dominated by 35 (Item) and 64 (Skill), whereas
/// `content + 16` has 6573 -- it only coincidentally agrees for some objects.
///
/// @par Why there are two kinds of string
/// An object's strings come from the cntc's own `strings` table via its
/// `PackContentStringIndexFixup` entries, and they are of two kinds. Measured
/// over one live cntc, 21997 strings = 20316 identifier slugs + 1681 readable
/// English labels that are *shared* across objects ("Damage Multiplier" x170).
/// Per-object counts are 0, 1, or 2..144 -- so neither "one string per object"
/// nor "the first string is the name" holds, and in multi-string objects the
/// slug is usually **last**. They must be told apart by shape, not position.
struct ContentObject {
    uint32_t type = 0;               ///< Content type number; see ::content_type_name.
    uint32_t id = 0;                 ///< Numeric id within the type.
    std::vector<uint32_t> assets;    ///< Referenced dat asset fileIds (model, textures, sounds).

    /// @brief The object's identifier slug, e.g. `"vl8Av.4gynM"`.
    ///
    /// An ArenaNet *internal* identifier, not a localized display name.
    /// Empty when the object has no slug-shaped string.
    std::string name;

    /// @brief The readable strings: parameter and condition names.
    ///
    /// Shared across objects, so they are context rather than identity
    /// ("Within Range", "AggroRadius", "/Roles[1]/LiveGestureCharrMale").
    std::vector<std::string> labels;
};

/// @brief Human label for a GW2 content type number.
///
/// IDA-verified where known (e.g. `CONTENT_TYPE_MINIPET == 48`); unknown types
/// come back as `"Type N"`.
const char* content_type_name(uint32_t type);

/// @brief True when @p s has the shape of a cntc identifier slug (`xxxxx.yyyyy`).
///
/// Base64-ish, no spaces, 3-8 characters either side of a single dot. This is
/// the classifier that separates ContentObject::name from
/// ContentObject::labels.
///
/// @warning A readable label that happens to look like `word.word` is
///          misclassified as a slug. No such collision has been observed in
///          practice, but the rule is a heuristic, not a guarantee.
bool is_content_slug(const std::string& s);

/// @}
