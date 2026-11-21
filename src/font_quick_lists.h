#pragma once

#include "font_family_selection_model.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace font_quick_lists {

inline constexpr char RecentOption[] = "Subtitle/Font/Recent";
inline constexpr char RecentLimitOption[] = "Subtitle/Font/Recent Limit";
inline constexpr char FavoritesOption[] = "Subtitle/Font/Favorites";
inline constexpr int MaxRecent = 100;

using Names = std::vector<std::string>;

bool IsFontNameBlank(std::string_view name);

Names TrimRecent(Names const& names, std::int64_t limit);
Names RecordRecent(Names const& names, std::string_view selected, std::int64_t limit);
void RemoveRecent(Names& names, std::string_view selected);

// A zero ID represents a favorite saved by its literal bare name.
FontFamilyChoice FavoriteIdentity(FontFamilySelectionModel const& model,
								  std::string_view label, FontFamilyId explicit_family = 0);
bool IsFavorite(FontFamilySelectionModel const& model, Names const& names, FontFamilyChoice const& choice);
void ToggleFavorite(FontFamilySelectionModel const& model, Names& names, FontFamilyChoice const& choice);
void RemoveFavorite(FontFamilySelectionModel const& model, Names& names, FontFamilyChoice const& choice);
std::vector<FontFamilyChoice> FavoriteRows(FontFamilySelectionModel const& model, Names const& names);
std::vector<FontFamilyChoice> FavoriteChoices(FontFamilySelectionModel const& model, Names const& names);

} // namespace font_quick_lists
