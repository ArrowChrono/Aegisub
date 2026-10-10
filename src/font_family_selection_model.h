#pragma once

#include "font_family_catalog.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/// One item in a platform-neutral font family selector. A zero family id
/// identifies an enumerator fallback which is not backed by a catalog record.
struct FontFamilyChoice {
	std::string label;
	FontFamilyId family_id = 0;

	bool operator==(FontFamilyChoice const&) const = default;
};

/// How vertical ('@') faces appear in custom font selectors.
enum class VerticalFontUiMode : std::uint8_t {
	/// Default: list both horizontal and GDI '@' faces (system-font-dialog-like).
	SystemList = 0,
	/// Compact: horizontal list only; a Vertical checkbox when GDI registered '@'.
	CompactToggle = 1,
};

/// Font family names and catalog state shared by GUI font selectors. This
/// model deliberately has no dependency on wxWidgets or a platform resolver.
struct FontFamilySelectionModel {
	bool prefer_localized = true;
	VerticalFontUiMode vertical_ui_mode = VerticalFontUiMode::SystemList;
	std::shared_ptr<FontFamilyCatalog const> catalog;
	std::vector<FontFamilyChoice> choices;
	/// Catalog family ids for which GDI registered a leading-'@' face.
	std::unordered_set<FontFamilyId> vertical_capable_family_ids;
	/// Bare face strings (no '@') with GDI vertical when family id is unavailable.
	std::unordered_set<std::string> vertical_capable_bare_names;

	bool UsesCompactVerticalToggle() const noexcept {
		return vertical_ui_mode == VerticalFontUiMode::CompactToggle;
	}

	/// True when GDI advertised a vertical face for this family / bare name.
	/// Unknown faces absent from both the catalog and live GDI set return false.
	bool SupportsVerticalWriting(
		FontFamilyId family_id,
		std::string_view face_name) const;

	std::string PreferredName(std::string_view stored_name) const;

	/// Resolve a family through the same alias model used by every font UI.
	FontFamilyRecord const* ResolveRecord(std::string_view name) const;

	/// Resolve an item selected from choices without going back through its
	/// potentially ambiguous display alias.
	FontFamilyRecord const* ResolveChoice(FontFamilyId family_id) const;
};

/// Build catalog-backed choices, or retain the supplied ordered fallback list
/// when no catalog snapshot is available.
FontFamilySelectionModel BuildFontFamilySelectionModel(
	std::shared_ptr<FontFamilyCatalog const> catalog,
	bool prefer_localized,
	std::vector<std::string> fallback_names = {});

/// Record GDI '@' faces as vertical capability (family id when resolvable, else bare).
void FillVerticalCapability(
	FontFamilySelectionModel& model,
	std::vector<std::string> const& vertical_faces);

/// Mode A: append preferred '@' labels for GDI vertical faces (dedupe + sort).
void AppendVerticalFacenameChoices(
	FontFamilySelectionModel& model,
	std::vector<std::string> const& vertical_faces);

/// List labels in compact mode are deliberately bare, so a list commit keeps
/// the current checkbox intent. Manual edits instead take intent from '@'.
bool UpdateVerticalWritingIntent(
	bool current_intent,
	std::string_view face_name,
	bool committing_list_selection) noexcept;
