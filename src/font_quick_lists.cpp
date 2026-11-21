#include "font_quick_lists.h"

#include <libaegisub/string_utils.h>

#include <unicode/unistr.h>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace font_quick_lists {
namespace {

std::string Fold(std::string_view name) {
	if (name.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
		return std::string(name);
	auto text = icu::UnicodeString::fromUTF8(icu::StringPiece(name.data(), static_cast<int32_t>(name.size())));
	std::string folded;
	return text.foldCase().toUTF8String(folded);
}

std::string Bare(std::string_view name) {
	return FontFamilyCatalog::SplitVerticalPrefix(name).second;
}

FontFamilyId ResolvedFamily(FontFamilySelectionModel const& model, std::string_view bare_name) {
	auto const *record = model.ResolveRecord(bare_name);
	return record ? record->id : 0;
}

bool MatchesFavorite(FontFamilySelectionModel const& model, std::string_view stored, FontFamilyChoice const& choice) {
	auto const bare = Bare(stored);
	auto const id = ResolvedFamily(model, bare);
	if (id != choice.family_id)
		return false;
	return id != 0 || Fold(bare) == Fold(choice.label);
}

} // namespace

bool IsFontNameBlank(std::string_view name) {
	return agi::util::strings::trim_utf8_copy(name).empty();
}

Names TrimRecent(Names const& names, std::int64_t limit) {
	auto const count = static_cast<std::size_t>(std::clamp<std::int64_t>(limit, 0, MaxRecent));
	Names result;
	std::unordered_set<std::string> seen;
	for (auto const& name : names) {
		if (result.size() == count)
			break;
		if (!IsFontNameBlank(name) && seen.insert(Fold(name)).second)
			result.push_back(name);
	}
	return result;
}

Names RecordRecent(Names const& names, std::string_view selected, std::int64_t limit) {
	auto recent = names;
	if (!IsFontNameBlank(selected))
		recent.insert(recent.begin(), std::string(selected));
	return TrimRecent(recent, limit);
}

void RemoveRecent(Names& names, std::string_view selected) {
	auto const key = Fold(selected);
	std::erase_if(names, [&](std::string const& name) { return Fold(name) == key; });
}

FontFamilyChoice FavoriteIdentity(FontFamilySelectionModel const& model, std::string_view label, FontFamilyId explicit_family) {
	auto bare = Bare(label);
	if (IsFontNameBlank(bare))
		return {};
	auto const *record = model.ResolveChoice(explicit_family);
	if (!record)
		record = model.ResolveRecord(bare);
	if (!record)
		return {.label = std::move(bare)};

	auto const preferred = model.catalog->PreferredWriteName(*record, model.prefer_localized);
	auto unique = [&](std::string_view alias) { return !IsFontNameBlank(alias) && ResolvedFamily(model, alias) == record->id; };
	for (std::string const *candidate : {&preferred, &record->localized_family_name, &record->english_win32_family_name, &std::as_const(bare)}) {
		auto alias = Bare(*candidate);
		if (unique(alias))
			return {.label = std::move(alias), .family_id = record->id};
	}
	for (auto const& name : record->names) {
		auto alias = Bare(name.value);
		if (name.kind == FontFamilyNameKind::Win32Family && unique(alias))
			return {.label = std::move(alias), .family_id = record->id};
	}
	return {.label = std::move(bare)};
}

bool IsFavorite(FontFamilySelectionModel const& model, Names const& names, FontFamilyChoice const& choice) {
	return !choice.label.empty() && std::ranges::any_of(names, [&](auto const& name) { return MatchesFavorite(model, name, choice); });
}

void RemoveFavorite(FontFamilySelectionModel const& model, Names& names, FontFamilyChoice const& choice) {
	std::erase_if(names, [&](auto const& name) { return IsFontNameBlank(Bare(name)) || MatchesFavorite(model, name, choice); });
}

void ToggleFavorite(FontFamilySelectionModel const& model, Names& names, FontFamilyChoice const& choice) {
	if (choice.label.empty())
		return;
	if (IsFavorite(model, names, choice))
		RemoveFavorite(model, names, choice);
	else {
		std::erase_if(names, [](auto const& name) { return IsFontNameBlank(Bare(name)); });
		names.push_back(choice.label);
	}
}

std::vector<FontFamilyChoice> FavoriteRows(FontFamilySelectionModel const& model, Names const& names) {
	std::vector<FontFamilyChoice> rows;
	std::unordered_set<FontFamilyId> families;
	std::unordered_set<std::string> literals;
	for (auto const& name : names) {
		auto item = FavoriteIdentity(model, name);
		if (!item.label.empty() && (item.family_id ? families.insert(item.family_id).second : literals.insert(Fold(item.label)).second))
			rows.push_back(std::move(item));
	}
	return rows;
}

std::vector<FontFamilyChoice> FavoriteChoices(FontFamilySelectionModel const& model, Names const& names) {
	auto favorites = FavoriteRows(model, names);
	bool const has_catalog = model.catalog && !model.catalog->empty();
	std::vector<bool> horizontal(favorites.size()), vertical(favorites.size());
	std::vector<FontFamilyChoice> choices;
	for (auto const& choice : model.choices) {
		auto const [prefix, bare] = FontFamilyCatalog::SplitVerticalPrefix(choice.label);
		auto const favorite = std::ranges::find_if(favorites, [&](FontFamilyChoice const& item) {
			if (has_catalog)
				return item.family_id != 0 && item.family_id == choice.family_id;
			return Fold(item.label) == Fold(bare);
		});
		if (favorite == favorites.end())
			continue;
		auto const index = static_cast<std::size_t>(favorite - favorites.begin());
		bool const use_vertical = !prefix.empty() && !model.UsesCompactVerticalToggle();
		auto& seen = use_vertical ? vertical : horizontal;
		if (seen[index])
			continue;
		seen[index] = true;
		choices.push_back({.label = FontFamilyCatalog::JoinVerticalPrefix(use_vertical, has_catalog ? favorite->label : bare),
						   .family_id = favorite->family_id});
	}
	for (std::size_t index = 0; index < favorites.size(); ++index) {
		if (!horizontal[index] && !vertical[index])
			choices.push_back(std::move(favorites[index]));
	}
	return choices;
}

} // namespace font_quick_lists
