#include "font_portability_audit.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"

#include <algorithm>
#include <iterator>

namespace {

bool OptionsMatch(
	FontPortabilityAuditPlan const& plan,
	FontPortabilityAuditOptions const& options) {
	return plan.options.name_target == plan.names.target &&
	       plan.options.variant == plan.variants.options &&
	       options == plan.options;
}

} // namespace

void FontPortabilityAuditSelection::Reset(
	FontPortabilityAuditPlan const& plan,
	FontPortabilityAuditFilter initial_filter) {
	ResetNames(plan.names, initial_filter != FontPortabilityAuditFilter::Variants);
	ResetVariants(plan.variants, initial_filter != FontPortabilityAuditFilter::Names);
}

void FontPortabilityAuditSelection::ResetNames(
	FontNameNormalizationPlan const& plan,
	bool select_safe) {
	names.assign(plan.changes.size(), 0);
	if (!select_safe)
		return;
	for (std::size_t i = 0; i < plan.changes.size(); ++i)
		names[i] = plan.changes[i].safe_to_apply;
}

void FontPortabilityAuditSelection::ResetVariants(
	FontVariantAuditPlan const& plan,
	bool select_safe) {
	variants.assign(plan.findings.size(), 0);
	if (!select_safe)
		return;
	for (std::size_t i = 0; i < plan.findings.size(); ++i)
		variants[i] = plan.findings[i].safe_to_apply;
}

void FontPortabilityAuditSelection::SelectSafe(
	FontPortabilityAuditPlan const& plan,
	FontPortabilityAuditFilter filter,
	bool select) {
	if (filter != FontPortabilityAuditFilter::Variants)
		ResetNames(plan.names, select);
	if (filter != FontPortabilityAuditFilter::Names)
		ResetVariants(plan.variants, select);
}

bool FontPortabilityAuditSelection::IsNameSelected(std::size_t index) const {
	return index < names.size() && names[index] != 0;
}

bool FontPortabilityAuditSelection::IsVariantSelected(std::size_t index) const {
	return index < variants.size() && variants[index] != 0;
}

bool FontPortabilityAuditSelection::SetNameSelected(
	FontNameNormalizationPlan const& plan,
	std::size_t index,
	bool selected) {
	if (index >= names.size() || index >= plan.changes.size())
		return false;
	auto const accepted = selected && plan.changes[index].safe_to_apply;
	names[index] = accepted;
	return accepted;
}

bool FontPortabilityAuditSelection::SetVariantSelected(
	FontVariantAuditPlan const& plan,
	std::size_t index,
	bool selected) {
	if (index >= variants.size() || index >= plan.findings.size())
		return false;
	auto const accepted = selected && plan.findings[index].safe_to_apply;
	variants[index] = accepted;
	return accepted;
}

std::size_t FontPortabilityAuditSelection::SelectedNameCount() const {
	return static_cast<std::size_t>(
		std::count(names.begin(), names.end(), std::uint8_t{1}));
}

std::size_t FontPortabilityAuditSelection::SelectedVariantCount() const {
	return static_cast<std::size_t>(
		std::count(variants.begin(), variants.end(), std::uint8_t{1}));
}

bool FontPortabilityAuditSelection::HasSelection() const {
	return SelectedNameCount() != 0 || SelectedVariantCount() != 0;
}

std::vector<std::size_t> FontPortabilityAuditSelection::SelectedNameIndices() const {
	std::vector<std::size_t> selected;
	selected.reserve(SelectedNameCount());
	for (std::size_t i = 0; i < names.size(); ++i) {
		if (names[i])
			selected.push_back(i);
	}
	return selected;
}

std::vector<std::size_t> FontPortabilityAuditSelection::SelectedVariantIndices() const {
	std::vector<std::size_t> selected;
	selected.reserve(SelectedVariantCount());
	for (std::size_t i = 0; i < variants.size(); ++i) {
		if (variants[i])
			selected.push_back(i);
	}
	return selected;
}

FontPortabilityAuditPlan BuildFontPortabilityAuditPlan(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	FontPortabilityAuditOptions options,
	std::span<int const> style_source_lines,
	FontVariantAuditProfileProvider const& profile_provider) {
	FontPortabilityAuditPlan plan;
	plan.options = options;
	plan.names = BuildFontNameNormalizationPlan(
		file, catalog, options.name_target, style_source_lines);
	plan.variants = BuildFontVariantAuditPlan(
		file, catalog, options.variant, style_source_lines, profile_provider);
	for (std::size_t index = 0; index < plan.names.changes.size(); ++index) {
		auto& change = plan.names.changes[index];
		if (!change.safe_to_apply)
			continue;
		AssFile staged;
		CloneFontAuditEntries(file, staged);
		auto const preflight = ApplyFontPortabilityAuditChanges(
			staged, plan, std::span(&index, 1), {}, options, profile_provider);
		if (!preflight.success) {
			change.safe_to_apply = false;
			change.reason_code = "font_state_preservation_unproven";
		}
	}
	return plan;
}

FontPortabilityAuditApplyResult ApplyFontPortabilityAuditChanges(
	AssFile& file,
	FontPortabilityAuditPlan const& plan,
	std::span<std::size_t const> selected_name_change_indices,
	std::span<std::size_t const> selected_variant_finding_indices,
	FontPortabilityAuditOptions options,
	FontVariantAuditProfileProvider const& profile_provider) {
	FontPortabilityAuditApplyResult result;
	if (!OptionsMatch(plan, options)) {
		result.error = "font portability audit options changed after the plan was built";
		return result;
	}

	AssFile staged;
	CloneFontAuditEntries(file, staged);

	// Variant snapshots include the original family name, while name changes do
	// not depend on weight/italic. This order allows both plans to target the
	// same style or \fn span without weakening either plan's stale-state checks.
	auto variant_result = ApplyFontVariantAuditChanges(
		staged,
		plan.variants,
		selected_variant_finding_indices,
		options.variant,
		profile_provider);
	if (!variant_result.success) {
		result.error = "font variant audit failed: " + variant_result.error;
		return result;
	}

	auto name_result = ApplyFontNameNormalizationChanges(
		staged, plan.names, selected_name_change_indices);
	if (!name_result.success) {
		result.error = "font name audit failed: " + name_result.error;
		return result;
	}
	if (!PreservesFontSelections(file, staged, plan.variants.catalog_snapshot, profile_provider, true)) {
		result.error = "font portability audit cannot preserve every affected font selection";
		return result;
	}

	if (file.Styles.size() != staged.Styles.size() ||
	    file.Events.size() != staged.Events.size()) {
		result.error = "font portability audit changed the subtitle structure";
		return result;
	}

	auto staged_style = staged.Styles.begin();
	for (auto& style : file.Styles) {
		if (style.font != staged_style->font ||
		    style.bold != staged_style->bold ||
		    style.italic != staged_style->italic ||
		    style.GetEntryData() != staged_style->GetEntryData()) {
			style.SwapFontState(*staged_style);
			result.styles_changed = true;
		}
		++staged_style;
	}

	auto staged_event = staged.Events.begin();
	for (auto& event : file.Events) {
		if (event.Text != staged_event->Text) {
			event.Text.swap(staged_event->Text);
			result.dialogue_text_changed = true;
		}
		++staged_event;
	}

	result.applied_name_change_count = name_result.applied_change_count;
	result.applied_variant_change_count = variant_result.applied_change_count;
	result.success = true;
	return result;
}
