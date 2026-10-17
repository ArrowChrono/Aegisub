#pragma once

#include "font_family_catalog.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

class AssFile;

namespace aegisub::ass {
struct AssFontRequest;
}

enum class FontVariantAuditSourceKind {
	Style,
	OverrideSpan
};

enum class FontVariantAuditClassification {
	ImplicitBoldFallback,
	ImplicitItalicFallback,
	ImplicitBoldItalicFallback,
	NonCanonical,
	Synthetic,
	Unknown
};

struct FontVariantAuditOptions {
	/// Explicit \b/\i tags are report-only unless this is enabled for the
	/// current plan/apply call.
	bool allow_replace_explicit = false;

	bool operator==(FontVariantAuditOptions const&) const = default;
};

struct FontVariantAuditSource {
	FontVariantAuditSourceKind kind = FontVariantAuditSourceKind::Style;
	std::string style;
	int line = 0;
	std::size_t entry_index = 0;
	/// Index in AssDialogue::ParseTags(). Only used for OverrideSpan sources.
	std::size_t block_index = 0;
	/// Stable depth-first index of the explicit \fn tag in the event.
	std::size_t fn_override_index = 0;
	/// Increments whenever a font-affecting state tag is encountered. Spans
	/// with the same revision share one effective request even when unrelated
	/// override blocks occur between them.
	std::size_t font_state_revision = 0;
	bool comment = false;
	bool in_transform = false;
};

/// Source state captured while building a plan. Keeping the relevant fields
/// instead of relying on list iterators makes stale-source checks deterministic.
struct FontVariantAuditSourceSnapshot {
	FontVariantAuditSourceKind kind = FontVariantAuditSourceKind::Style;
	std::size_t entry_index = 0;
	std::string style;
	std::string family;
	std::string text;
	int weight = 400;
	bool italic = false;
	int charset = 1;
	double height = 0.0;
	bool comment = false;
};

struct FontVariantAuditFinding {
	FontVariantAuditSource source;
	std::size_t snapshot_index = 0;

	std::string family;
	FontFamilyId family_id = 0;
	FontFamilyMatchKind match_kind = FontFamilyMatchKind::None;
	FontVariantAuditClassification classification = FontVariantAuditClassification::Unknown;
	FontVariantStatus status = FontVariantStatus::Unknown;
	FontVariantBackend profile_backend = FontVariantBackend::Unknown;
	FontSelectionEvidence profile_evidence = FontSelectionEvidence::None;
	FontVariantRole realized_role = FontVariantRole::Unknown;
	std::uint64_t entity_token = 0;

	int requested_weight = 400;
	bool requested_italic = false;
	int charset = 1;
	double height = 0.0;
	bool has_explicit_bold = false;
	bool has_explicit_italic = false;
	std::string raw_bold_tag;
	std::string raw_italic_tag;

	/// Safe fixes only add the missing RBIZ bits. They never silently turn a
	/// requested Bold/Italic bit off.
	bool pin_bold = false;
	bool pin_italic = false;
	bool requires_explicit_replacement = false;
	bool safe_to_apply = false;
	std::string reason_code;
};

struct FontVariantAuditPlan {
	FontVariantAuditOptions options;
	FontFamilyCatalog catalog_snapshot;
	bool catalog_available = false;
	std::size_t scanned_style_count = 0;
	std::size_t scanned_override_span_count = 0;
	std::vector<FontVariantAuditSourceSnapshot> snapshots;
	std::vector<FontVariantAuditFinding> findings;
};

struct FontVariantAuditApplyResult {
	bool success = false;
	bool styles_changed = false;
	bool dialogue_text_changed = false;
	std::size_t applied_change_count = 0;
	std::string error;
};

/// Supplies a request-specific physical profile. On Windows the UI provides a
/// fresh GDI-backed instance for both scanning and applying. Tests can inject
/// deterministic profiles; an empty provider preserves the catalog profile
/// behavior used by non-Windows callers.
using FontVariantAuditProfileProvider = std::function<
	std::optional<FontFamilyVariantProfile>(aegisub::ass::AssFontRequest const&)>;

/// Scan style definitions and text spans introduced by explicit \fn tags.
/// This function never modifies the AssFile.
FontVariantAuditPlan BuildFontVariantAuditPlan(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	FontVariantAuditOptions options = {},
	std::span<int const> style_source_lines = {},
	FontVariantAuditProfileProvider const& profile_provider = {});

/// Apply selected safe findings after validating every source. Validation is
/// all-or-nothing: a stale style, event, or span prevents every mutation.
FontVariantAuditApplyResult ApplyFontVariantAuditChanges(
	AssFile& file,
	FontVariantAuditPlan const& plan,
	std::span<std::size_t const> selected_finding_indices,
	FontVariantAuditOptions options = {},
	FontVariantAuditProfileProvider const& profile_provider = {});

bool PreservesFontSelections(
	AssFile const& original,
	AssFile const& candidate,
	FontFamilyCatalog const& catalog,
	FontVariantAuditProfileProvider const& profile_provider = {},
	bool allow_family_alias_changes = false);

void CloneFontAuditEntries(AssFile const& source, AssFile& destination);
