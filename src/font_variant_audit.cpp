#include "font_variant_audit.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_font_state.h"
#include "ass_style.h"
#include "ass_style_resolution.h"
#include "font_variant_policy.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace {

using aegisub::ass::AssFontRequest;
using aegisub::ass::AssFontStateEvaluator;
using aegisub::ass::AssFontStyleBaseline;

AssStyle const* StyleAt(AssFile const& file, std::size_t index) {
	if (index >= file.Styles.size())
		return nullptr;
	auto it = file.Styles.begin();
	std::advance(it, static_cast<std::ptrdiff_t>(index));
	return &*it;
}

AssStyle* StyleAt(AssFile& file, std::size_t index) {
	return const_cast<AssStyle*>(StyleAt(static_cast<AssFile const&>(file), index));
}

AssDialogue const* EventAt(AssFile const& file, std::size_t index) {
	if (index >= file.Events.size())
		return nullptr;
	auto it = file.Events.begin();
	std::advance(it, static_cast<std::ptrdiff_t>(index));
	return &*it;
}

AssDialogue* EventAt(AssFile& file, std::size_t index) {
	return const_cast<AssDialogue*>(EventAt(static_cast<AssFile const&>(file), index));
}

bool IsExplicitNonEmptyFontTag(AssOverrideTag const& tag) {
	if (tag.Name != "\\fn" || tag.Params.empty())
		return false;
	auto const& parameter = tag.Params.front();
	return !parameter.omitted && !parameter.empty &&
	       !parameter.Get<std::string>().empty();
}

struct ExplicitFontSpan {
	FontVariantAuditSource source;
	AssFontRequest request;
	std::string text;
};

struct FontSpanScanResult {
	std::vector<ExplicitFontSpan> spans;
	std::vector<AssFontStyleBaseline> baselines;
	bool complete = true;
	bool unverifiable_font_state = false;
};

class FontSpanScanner {
	AssFile const& file;
	AssDialogue const& event;
	std::size_t event_index;
	AssFontStateEvaluator evaluator;
	std::size_t next_fn_index = 0;
	std::size_t font_state_revision = 0;

	struct ActiveFontOverride {
		std::size_t index = 0;
		bool in_transform = false;
	};
	std::optional<ActiveFontOverride> active_font;
	bool bold_in_transform = false;
	bool italic_in_transform = false;
	FontSpanScanResult result;

	std::optional<AssFontStyleBaseline> ResolveResetStyle(std::string_view name) {
		auto const* style = aegisub::ass_style_resolution::ResolveResetStyle(
			file, std::string(name));
		if (!style) {
			result.complete = false;
			return std::nullopt;
		}
		auto baseline = aegisub::ass::MakeAssFontStyleBaseline(*style);
		result.baselines.push_back(baseline);
		return baseline;
	}

	void ApplyTags(std::vector<AssOverrideTag> const& tags, bool in_transform) {
		for (auto const& tag : tags) {
			bool const changes_font = tag.Name == "\\r" || tag.Name == "\\fn" ||
									  tag.Name == "\\b" || tag.Name == "\\i" ||
									  tag.Name == "\\fe" || tag.Name == "\\fs";
			if (in_transform && changes_font)
				result.unverifiable_font_state = true;
			if (tag.Name == "\\fs" && !tag.Params.empty() &&
				!tag.Params.front().omitted && !tag.Params.front().empty) {
				auto const value = tag.Params.front().Get<std::string>();
				if (!value.empty() && (value.front() == '+' || value.front() == '-'))
					result.unverifiable_font_state = true;
			}
			if (tag.Name == "\\t" && tag.Params.size() > 3 &&
			    !tag.Params[3].omitted && !tag.Params[3].empty) {
				if (auto const* nested = tag.Params[3].Get<AssDialogueBlockOverride*>())
					ApplyTags(nested->Tags, true);
				continue;
			}

			evaluator.ApplyTag(tag, [this](std::string_view name) {
				return ResolveResetStyle(name);
			});

			if (tag.Name == "\\r") {
				active_font.reset();
				bold_in_transform = in_transform;
				italic_in_transform = in_transform;
			}
			else if (tag.Name == "\\fn") {
				auto const current_index = next_fn_index++;
				if (IsExplicitNonEmptyFontTag(tag))
					active_font = ActiveFontOverride{current_index, in_transform};
				else
					active_font.reset();
			}
			else if (tag.Name == "\\b")
				bold_in_transform = in_transform;
			else if (tag.Name == "\\i")
				italic_in_transform = in_transform;

			if (tag.Name == "\\r" || tag.Name == "\\fn" ||
			    tag.Name == "\\b" || tag.Name == "\\i" ||
			    tag.Name == "\\fe" || tag.Name == "\\fs")
				++font_state_revision;
		}
	}

public:
FontSpanScanner(
	AssFile const& file,
	AssDialogue const& event,
	std::size_t event_index,
	AssFontStyleBaseline event_style)
	: file(file), event(event), event_index(event_index), evaluator(std::move(event_style)) {
	result.baselines.push_back(evaluator.EventStyle());
}

FontSpanScanResult Scan(bool explicit_only) {
	auto blocks = event.ParseTags();
	for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
		auto const& block = blocks[block_index];
		if (block->GetType() == AssBlockType::OVERRIDE) {
			ApplyTags(static_cast<AssDialogueBlockOverride const&>(*block).Tags, false);
			continue;
		}

		if (block->GetType() != AssBlockType::PLAIN || block->GetText().empty())
			continue;
		if (!evaluator.IsValid()) {
			result.complete = false;
			continue;
		}
		if (explicit_only && !active_font)
			continue;

		ExplicitFontSpan span;
		span.source.kind = FontVariantAuditSourceKind::OverrideSpan;
		span.source.style = event.Style;
		span.source.line = event.Row >= 0
							   ? event.Row + 1
							   : static_cast<int>(event_index + 1);
		span.source.entry_index = event_index;
		span.source.block_index = block_index;
		if (active_font)
			span.source.fn_override_index = active_font->index;
		span.source.font_state_revision = font_state_revision;
		span.source.comment = event.Comment;
		span.source.in_transform = (active_font && active_font->in_transform) ||
								   bold_in_transform || italic_in_transform;
		span.request = evaluator.Request();
		span.text = block->GetText();
		result.spans.push_back(std::move(span));
	}
	return std::move(result);
}
};

std::vector<ExplicitFontSpan> ScanExplicitFontSpans(
	AssFile const& file,
	AssDialogue const& event,
	std::size_t event_index) {
	auto const* style = aegisub::ass_style_resolution::ResolveEventStyle(file, event.Style);
	if (!style)
		return {};
	return FontSpanScanner(
			   file, event, event_index, aegisub::ass::MakeAssFontStyleBaseline(*style))
		.Scan(true)
		.spans;
}

std::size_t AddSnapshot(
	FontVariantAuditPlan& plan,
	FontVariantAuditSourceSnapshot snapshot) {
	for (std::size_t i = 0; i < plan.snapshots.size(); ++i) {
		auto const& existing = plan.snapshots[i];
		if (existing.kind == snapshot.kind &&
		    existing.entry_index == snapshot.entry_index)
			return i;
	}
	plan.snapshots.push_back(std::move(snapshot));
	return plan.snapshots.size() - 1;
}

FontVariantAuditSourceSnapshot StyleSnapshot(AssStyle const& style, std::size_t index) {
	FontVariantAuditSourceSnapshot snapshot;
	snapshot.kind = FontVariantAuditSourceKind::Style;
	snapshot.entry_index = index;
	snapshot.style = style.name;
	snapshot.family = style.font;
	snapshot.weight = style.bold
		? aegisub::ass::BoldFontWeight
		: aegisub::ass::DefaultFontWeight;
	snapshot.italic = style.italic;
	snapshot.charset = style.encoding;
	snapshot.height = style.fontsize;
	return snapshot;
}

FontVariantAuditSourceSnapshot EventSnapshot(AssDialogue const& event, std::size_t index) {
	FontVariantAuditSourceSnapshot snapshot;
	snapshot.kind = FontVariantAuditSourceKind::OverrideSpan;
	snapshot.entry_index = index;
	snapshot.style = event.Style;
	snapshot.text = event.Text.get();
	snapshot.comment = event.Comment;
	return snapshot;
}

void PopulateRequest(
	FontVariantAuditFinding& finding,
	AssFontRequest const& request) {
	finding.family = request.family;
	finding.requested_weight = request.effective_weight;
	finding.requested_italic = request.italic;
	finding.charset = request.charset;
	finding.height = request.height;
	finding.has_explicit_bold = request.has_explicit_bold;
	finding.has_explicit_italic = request.has_explicit_italic;
	finding.raw_bold_tag = request.raw_bold_tag;
	finding.raw_italic_tag = request.raw_italic_tag;
}

void SetUnknown(
	FontVariantAuditPlan& plan,
	FontVariantAuditFinding finding,
	std::string reason) {
	finding.classification = FontVariantAuditClassification::Unknown;
	finding.status = FontVariantStatus::Unknown;
	finding.reason_code = std::move(reason);
	plan.findings.push_back(std::move(finding));
}

void SetNonCanonical(
	FontVariantAuditPlan& plan,
	FontVariantAuditFinding finding,
	FontVariantOutcome const* outcome = nullptr,
	std::string reason = "noncanonical_variant") {
	finding.classification = FontVariantAuditClassification::NonCanonical;
	finding.status = outcome ? outcome->status : FontVariantStatus::NonCanonical;
	if (outcome) {
		finding.realized_role = outcome->role;
		finding.entity_token = outcome->entity_token;
	}
	finding.reason_code = std::move(reason);
	plan.findings.push_back(std::move(finding));
}

bool IsImplicitClassification(FontVariantAuditClassification classification) {
	return classification == FontVariantAuditClassification::ImplicitBoldFallback ||
	       classification == FontVariantAuditClassification::ImplicitItalicFallback ||
	       classification == FontVariantAuditClassification::ImplicitBoldItalicFallback;
}

void AnalyzeRequest(
	FontVariantAuditPlan& plan,
	FontFamilyCatalog const& catalog,
	FontVariantAuditProfileProvider const& profile_provider,
	FontVariantAuditOptions options,
	FontVariantAuditSource source,
	std::size_t snapshot_index,
	AssFontRequest const& request) {
	FontVariantAuditFinding finding;
	finding.source = std::move(source);
	finding.snapshot_index = snapshot_index;
	PopulateRequest(finding, request);

	if (catalog.empty()) {
		SetUnknown(plan, std::move(finding), "font_family_catalog_unavailable");
		return;
	}

	auto const resolved = catalog.Resolve(request.family);
	finding.match_kind = resolved.match;
	if (resolved.match == FontFamilyMatchKind::Ambiguous) {
		SetUnknown(plan, std::move(finding), "ambiguous_family_alias");
		return;
	}
	if ((resolved.match != FontFamilyMatchKind::Exact &&
	     resolved.match != FontFamilyMatchKind::CaseInsensitiveExact) ||
	    !resolved.family) {
		SetUnknown(plan, std::move(finding), "unrecognized_family_name");
		return;
	}

	auto const* record = catalog.Find(*resolved.family);
	if (!record) {
		SetUnknown(plan, std::move(finding), "unrecognized_family_name");
		return;
	}
	finding.family_id = record->id;

	// Exact numeric weights do not have a corresponding catalog probe. They
	// remain report-only even when replacing explicit overrides is permitted.
	if (request.effective_weight != aegisub::ass::DefaultFontWeight &&
	    request.effective_weight != aegisub::ass::BoldFontWeight) {
		finding.requires_explicit_replacement = request.has_explicit_bold;
		SetNonCanonical(plan, std::move(finding), nullptr, "numeric_weight_report_only");
		return;
	}

	std::optional<FontFamilyVariantProfile> request_profile;
	auto const* profile = &record->variant_profile;
	if (profile_provider) {
		request_profile = profile_provider(request);
		if (!request_profile) {
			SetUnknown(plan, std::move(finding), "variant_profile_unavailable");
			return;
		}
		profile = &*request_profile;
	}
	finding.profile_backend = profile->backend;
	finding.profile_evidence = profile->evidence;

	bool const requested_bold = request.effective_weight == aegisub::ass::BoldFontWeight;
	auto const& outcome = profile->For(requested_bold, request.italic);
	finding.status = outcome.status;
	finding.realized_role = outcome.role;
	finding.entity_token = outcome.entity_token;

	if (outcome.status == FontVariantStatus::Synthetic) {
		finding.classification = FontVariantAuditClassification::Synthetic;
		finding.reason_code = "synthetic_variant";
		plan.findings.push_back(std::move(finding));
		return;
	}
	if (outcome.status == FontVariantStatus::NonCanonical) {
		SetNonCanonical(plan, std::move(finding), &outcome);
		return;
	}
	if (outcome.status != FontVariantStatus::Canonical) {
		SetUnknown(plan, std::move(finding), "variant_profile_unavailable");
		return;
	}

	auto current_choice = CanonicalizeFontVariant(outcome);
	if (!current_choice) {
		SetUnknown(plan, std::move(finding), "variant_profile_unavailable");
		return;
	}
	if (current_choice->weight == request.effective_weight &&
	    current_choice->italic == request.italic)
		return;

	FontVariantSelection current;
	current.weight = request.effective_weight;
	current.italic = request.italic;
	current.has_explicit_bold = request.has_explicit_bold;
	current.has_explicit_italic = request.has_explicit_italic;
	auto candidate = AdjustFamilySelection(
		current, *profile, {true, true});

	bool const pin_bold = candidate.changed_weight &&
		current.weight == aegisub::ass::DefaultFontWeight &&
		candidate.selection.weight == aegisub::ass::BoldFontWeight;
	bool const pin_italic = candidate.changed_italic &&
		!current.italic && candidate.selection.italic;
	if (!candidate.applied_implicit_selection || (!pin_bold && !pin_italic) ||
	    (candidate.changed_weight && !pin_bold) ||
	    (candidate.changed_italic && !pin_italic)) {
		SetNonCanonical(plan, std::move(finding), &outcome);
		return;
	}

	bool const target_bold = candidate.selection.weight == aegisub::ass::BoldFontWeight;
	auto const& target_outcome = profile->For(
		target_bold, candidate.selection.italic);
	auto target_choice = CanonicalizeFontVariant(target_outcome);
	if (!target_choice || target_choice->entity_token != current_choice->entity_token ||
	    target_choice->role != current_choice->role) {
		SetNonCanonical(plan, std::move(finding), &outcome);
		return;
	}

	finding.pin_bold = pin_bold;
	finding.pin_italic = pin_italic;
	finding.requires_explicit_replacement =
		(pin_bold && request.has_explicit_bold) ||
		(pin_italic && request.has_explicit_italic);
	if (pin_bold && pin_italic)
		finding.classification = FontVariantAuditClassification::ImplicitBoldItalicFallback;
	else if (pin_bold)
		finding.classification = FontVariantAuditClassification::ImplicitBoldFallback;
	else
		finding.classification = FontVariantAuditClassification::ImplicitItalicFallback;

	if (finding.source.in_transform) {
		finding.reason_code = "transform_variant_report_only";
		finding.safe_to_apply = false;
	}
	else if (finding.requires_explicit_replacement && !options.allow_replace_explicit) {
		finding.reason_code = "explicit_variant_override_preserved";
		finding.safe_to_apply = false;
	}
	else {
		finding.safe_to_apply = true;
		if (finding.classification == FontVariantAuditClassification::ImplicitBoldItalicFallback)
			finding.reason_code = "implicit_bold_italic_fallback";
		else if (finding.classification == FontVariantAuditClassification::ImplicitBoldFallback)
			finding.reason_code = "implicit_bold_fallback";
		else
			finding.reason_code = "implicit_italic_fallback";
	}
	plan.findings.push_back(std::move(finding));
}

bool IsApplicable(
	FontVariantAuditFinding const& finding,
	FontVariantAuditOptions options) {
	if (!IsImplicitClassification(finding.classification) ||
	    (!finding.pin_bold && !finding.pin_italic) ||
	    finding.source.in_transform ||
	    finding.profile_evidence != FontSelectionEvidence::Observed)
		return false;
	return !finding.requires_explicit_replacement || options.allow_replace_explicit;
}

bool SnapshotMatches(AssFile const& file, FontVariantAuditSourceSnapshot const& snapshot) {
	if (snapshot.kind == FontVariantAuditSourceKind::Style) {
		auto const* style = StyleAt(file, snapshot.entry_index);
		return style && style->name == snapshot.style && style->font == snapshot.family &&
		       (style->bold ? aegisub::ass::BoldFontWeight : aegisub::ass::DefaultFontWeight) == snapshot.weight &&
		       style->italic == snapshot.italic && style->encoding == snapshot.charset &&
		       style->fontsize == snapshot.height;
	}

	auto const* event = EventAt(file, snapshot.entry_index);
	return event && event->Style == snapshot.style && event->Text.get() == snapshot.text &&
	       event->Comment == snapshot.comment;
}

bool RequestMatches(FontVariantAuditFinding const& finding, ExplicitFontSpan const& span) {
	auto const& request = span.request;
	return span.source.block_index == finding.source.block_index &&
	       span.source.fn_override_index == finding.source.fn_override_index &&
	       span.source.font_state_revision == finding.source.font_state_revision &&
	       span.source.in_transform == finding.source.in_transform &&
	       request.family == finding.family &&
	       request.effective_weight == finding.requested_weight &&
	       request.italic == finding.requested_italic &&
	       request.charset == finding.charset &&
	       request.height == finding.height &&
	       request.has_explicit_bold == finding.has_explicit_bold &&
	       request.has_explicit_italic == finding.has_explicit_italic &&
	       request.raw_bold_tag == finding.raw_bold_tag &&
	       request.raw_italic_tag == finding.raw_italic_tag;
}

AssFontRequest RequestFromFinding(FontVariantAuditFinding const& finding) {
	AssFontRequest request;
	request.family = finding.family;
	request.effective_weight = finding.requested_weight;
	request.italic = finding.requested_italic;
	request.charset = finding.charset;
	request.height = finding.height;
	request.has_explicit_bold = finding.has_explicit_bold;
	request.has_explicit_italic = finding.has_explicit_italic;
	request.raw_bold_tag = finding.raw_bold_tag;
	request.raw_italic_tag = finding.raw_italic_tag;
	return request;
}

bool LivePinStillMatches(
	FontVariantAuditFinding const& finding,
	FontVariantAuditProfileProvider const& profile_provider) {
	if (!profile_provider)
		return true;

	auto const request = RequestFromFinding(finding);
	auto profile = profile_provider(request);
	if (!profile || profile->evidence != FontSelectionEvidence::Observed ||
	    !profile->automatic_pinning_reliable ||
	    profile->backend != finding.profile_backend ||
	    profile->evidence != finding.profile_evidence)
		return false;

	auto const& current_outcome = profile->For(
		request.effective_weight == aegisub::ass::BoldFontWeight,
		request.italic);
	auto const current = CanonicalizeFontVariant(current_outcome);
	if (!current || current->entity_token != finding.entity_token ||
	    current->role != finding.realized_role)
		return false;

	int const target_weight = finding.pin_bold
		? aegisub::ass::BoldFontWeight
		: request.effective_weight;
	bool const target_italic = finding.pin_italic || request.italic;
	auto const& target_outcome = profile->For(
		target_weight == aegisub::ass::BoldFontWeight,
		target_italic);
	auto const target = CanonicalizeFontVariant(target_outcome);
	return target && target->entity_token == current->entity_token &&
	       target->role == current->role;
}

std::string FixTags(
	FontVariantAuditFinding const& finding,
	bool include_bold = true,
	bool include_italic = true) {
	std::string tags;
	if (finding.pin_bold && include_bold)
		tags += "\\b1";
	if (finding.pin_italic && include_italic)
		tags += "\\i1";
	return tags;
}

struct ReplacedExplicitVariants {
	bool bold = false;
	bool italic = false;
};

ReplacedExplicitVariants ReplaceExplicitVariantTags(
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks,
	FontVariantAuditFinding const& finding) {
	ReplacedExplicitVariants replaced;
	AssOverrideTag *bold_tag = nullptr;
	AssOverrideTag *italic_tag = nullptr;
	std::size_t next_fn_index = 0;
	bool active_font = false;

	auto process_tags = [&](auto&& self, std::vector<AssOverrideTag>& tags, bool in_transform) -> void {
		for (auto& tag : tags) {
			if (tag.Name == "\\t" && tag.Params.size() > 3 &&
			    !tag.Params[3].omitted && !tag.Params[3].empty) {
				if (auto *nested = tag.Params[3].Get<AssDialogueBlockOverride*>())
					self(self, nested->Tags, true);
				continue;
			}
			if (tag.Name == "\\r") {
				active_font = false;
				bold_tag = nullptr;
				italic_tag = nullptr;
				continue;
			}
			if (tag.Name == "\\fn") {
				auto const index = next_fn_index++;
				active_font = !in_transform &&
					index == finding.source.fn_override_index &&
					IsExplicitNonEmptyFontTag(tag);
				// Only tags after this target family are eligible for in-place
				// replacement. A preceding \b/\i may affect earlier text too; the
				// apply phase inserts a local override when no later tag exists.
				continue;
			}
			if (!active_font || in_transform)
				continue;
			if (tag.Name == "\\b")
				bold_tag = &tag;
			else if (tag.Name == "\\i")
				italic_tag = &tag;
		}
	};

	for (std::size_t index = 0;
	     index < finding.source.block_index && index < blocks.size(); ++index) {
		if (blocks[index]->GetType() == AssBlockType::OVERRIDE)
			process_tags(
				process_tags,
				static_cast<AssDialogueBlockOverride&>(*blocks[index]).Tags,
				false);
	}

	if (finding.pin_bold && finding.has_explicit_bold && bold_tag &&
	    !bold_tag->Params.empty()) {
		bold_tag->Params.front().Set<int>(1);
		replaced.bold = true;
	}
	if (finding.pin_italic && finding.has_explicit_italic && italic_tag &&
	    !italic_tag->Params.empty()) {
		italic_tag->Params.front().Set<int>(1);
		replaced.italic = true;
	}
	return replaced;
}

struct ObservedFontSelection {
	FontVariantBackend backend;
	FontVariantOutcome outcome;
};

FontVariantAuditProfileProvider CachedProfiles(FontVariantAuditProfileProvider const& provider) {
	if (!provider)
		return {};
	using Key = std::tuple<std::string, int, double>;
	return [provider, profiles = std::map<Key, std::optional<FontFamilyVariantProfile>>{}](AssFontRequest const& request) mutable {
		auto const key = Key(request.family, request.charset, request.height);
		auto [entry, inserted] = profiles.try_emplace(key);
		if (inserted)
			entry->second = provider(request);
		return entry->second;
	};
}

std::optional<ObservedFontSelection> ObservedSelection(
	AssFontRequest const& request,
	FontFamilyCatalog const& catalog,
	FontVariantAuditProfileProvider const& profile_provider) {
	auto const resolved = catalog.Resolve(request.family);
	if (!resolved.family)
		return std::nullopt;
	auto const *record = catalog.Find(*resolved.family);
	if (!record)
		return std::nullopt;
	auto profile = profile_provider
					   ? profile_provider(request)
					   : std::optional(record->variant_profile);
	if (!profile || profile->evidence != FontSelectionEvidence::Observed ||
		!profile->automatic_pinning_reliable || profile->backend == FontVariantBackend::Unknown)
		return std::nullopt;
	auto const& outcome = profile->For(
		request.effective_weight == aegisub::ass::BoldFontWeight, request.italic);
	if (outcome.status != FontVariantStatus::Canonical ||
		outcome.role == FontVariantRole::Unknown || !outcome.entity_token)
		return std::nullopt;
	return ObservedFontSelection{
		.backend = profile->backend,
		.outcome = outcome};
}

bool SameFontSelection(
	AssFontRequest const& before,
	AssFontRequest const& after,
	FontFamilyCatalog const& catalog,
	FontVariantAuditProfileProvider const& profile_provider,
	bool allow_family_alias_changes) {
	if (!before.valid || !after.valid)
		return false;
	if (before.family == after.family && before.effective_weight == after.effective_weight &&
		before.italic == after.italic && before.charset == after.charset && before.height == after.height)
		return true;
	if (before.charset != after.charset || before.height != after.height ||
		(before.effective_weight != after.effective_weight &&
		 (before.effective_weight != aegisub::ass::DefaultFontWeight ||
		  after.effective_weight != aegisub::ass::BoldFontWeight)) ||
		(before.italic && !after.italic))
		return false;
	if ((before.effective_weight != aegisub::ass::DefaultFontWeight &&
		 before.effective_weight != aegisub::ass::BoldFontWeight) ||
		(after.effective_weight != aegisub::ass::DefaultFontWeight &&
		 after.effective_weight != aegisub::ass::BoldFontWeight))
		return false;
	if (before.family != after.family) {
		if (!allow_family_alias_changes || !profile_provider ||
			FontFamilyCatalog::SplitVerticalPrefix(before.family).first !=
				FontFamilyCatalog::SplitVerticalPrefix(after.family).first)
			return false;
		auto const old_family = catalog.Resolve(before.family);
		auto const new_family = catalog.Resolve(after.family);
		if (!old_family.family || old_family.family != new_family.family)
			return false;
	}
	auto const old_selection = ObservedSelection(before, catalog, profile_provider);
	auto const new_selection = ObservedSelection(after, catalog, profile_provider);
	return old_selection && new_selection &&
		   old_selection->backend == new_selection->backend &&
		   old_selection->outcome.entity_token == new_selection->outcome.entity_token &&
		   old_selection->outcome.role == new_selection->outcome.role &&
		   old_selection->outcome.realized_weight == new_selection->outcome.realized_weight &&
		   old_selection->outcome.realized_italic == new_selection->outcome.realized_italic;
}

} // namespace

void CloneFontAuditEntries(AssFile const& source, AssFile& destination) {
	for (auto const& style : source.Styles) {
		auto copy = std::make_unique<AssStyle>(style);
		destination.Styles.push_back(*copy);
		copy.release();
	}
	for (auto const& event : source.Events) {
		auto copy = std::make_unique<AssDialogue>(
			static_cast<AssDialogueBase const&>(event));
		destination.Events.push_back(*copy);
		copy.release();
	}
}

bool PreservesFontSelections(
	AssFile const& original,
	AssFile const& candidate,
	FontFamilyCatalog const& catalog,
	FontVariantAuditProfileProvider const& profile_provider,
	bool allow_family_alias_changes) {
	if (original.Styles.size() != candidate.Styles.size() ||
		original.Events.size() != candidate.Events.size())
		return false;
	auto const cached_provider = CachedProfiles(profile_provider);
	bool changed = false;
	auto new_style = candidate.Styles.begin();
	for (auto const& style : original.Styles) {
		if (style.name != new_style->name)
			return false;
		auto const before = AssFontStateEvaluator(aegisub::ass::MakeAssFontStyleBaseline(style)).Request();
		auto const after = AssFontStateEvaluator(aegisub::ass::MakeAssFontStyleBaseline(*new_style)).Request();
		changed = changed || before != after;
		if (!SameFontSelection(before, after, catalog, cached_provider, allow_family_alias_changes))
			return false;
		++new_style;
	}
	bool const styles_changed = changed;
	auto new_event = candidate.Events.begin();
	for (auto const& event : original.Events) {
		if (event.Style != new_event->Style || event.Comment != new_event->Comment)
			return false;
		changed = changed || event.Text != new_event->Text;
		++new_event;
	}
	if (!changed)
		return true;
	new_event = candidate.Events.begin();
	std::size_t event_index = 0;
	for (auto const& event : original.Events) {
		if (!styles_changed && event.Text == new_event->Text) {
			++new_event;
			++event_index;
			continue;
		}
		auto const *old_style = aegisub::ass_style_resolution::ResolveEventStyle(original, event.Style);
		auto const *candidate_style = aegisub::ass_style_resolution::ResolveEventStyle(candidate, new_event->Style);
		if (!old_style || !candidate_style)
			return false;
		auto const before = FontSpanScanner(original, event, event_index,
											aegisub::ass::MakeAssFontStyleBaseline(*old_style))
								.Scan(false);
		auto const after = FontSpanScanner(candidate, *new_event, event_index,
										   aegisub::ass::MakeAssFontStyleBaseline(*candidate_style))
							   .Scan(false);
		if (!before.complete || !after.complete || before.spans.size() != after.spans.size())
			return false;
		if ((before.unverifiable_font_state || after.unverifiable_font_state) &&
			(event.Text != new_event->Text || before.baselines != after.baselines))
			return false;
		for (std::size_t i = 0; i < before.spans.size(); ++i) {
			if (before.spans[i].text != after.spans[i].text ||
				!SameFontSelection(before.spans[i].request, after.spans[i].request,
								   catalog, cached_provider, allow_family_alias_changes))
				return false;
		}
		++new_event;
		++event_index;
	}
	return true;
}

FontVariantAuditPlan BuildFontVariantAuditPlan(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	FontVariantAuditOptions options,
	std::span<int const> style_source_lines,
	FontVariantAuditProfileProvider const& profile_provider) {
	FontVariantAuditPlan plan;
	plan.options = options;
	plan.catalog_snapshot = catalog;
	plan.catalog_available = !catalog.empty();
	auto const cached_provider = CachedProfiles(profile_provider);

	std::size_t style_index = 0;
	for (auto const& style : file.Styles) {
		auto const snapshot_index = AddSnapshot(plan, StyleSnapshot(style, style_index));
		FontVariantAuditSource source;
		source.kind = FontVariantAuditSourceKind::Style;
		source.style = style.name;
		source.entry_index = style_index;
		if (style_index < style_source_lines.size())
			source.line = style_source_lines[style_index];

		AssFontRequest request;
		request.family = style.font;
		request.effective_weight = style.bold
			? aegisub::ass::BoldFontWeight
			: aegisub::ass::DefaultFontWeight;
		request.italic = style.italic;
		request.charset = style.encoding;
		request.height = style.fontsize;
		AnalyzeRequest(
			plan, catalog, cached_provider, options, std::move(source), snapshot_index, request);
		++plan.scanned_style_count;
		++style_index;
	}

	std::size_t event_index = 0;
	for (auto const& event : file.Events) {
		auto spans = ScanExplicitFontSpans(file, event, event_index);
		if (!spans.empty()) {
			auto const snapshot_index = AddSnapshot(plan, EventSnapshot(event, event_index));
			for (auto& span : spans) {
				AnalyzeRequest(
					plan, catalog, cached_provider, options, std::move(span.source), snapshot_index, span.request);
				++plan.scanned_override_span_count;
			}
		}
		++event_index;
	}

	for (std::size_t index = 0; index < plan.findings.size(); ++index) {
		auto& finding = plan.findings[index];
		if (!finding.safe_to_apply)
			continue;
		AssFile staged;
		CloneFontAuditEntries(file, staged);
		auto const preflight = ApplyFontVariantAuditChanges(
			staged, plan, std::span(&index, 1), options, cached_provider);
		if (!preflight.success) {
			finding.safe_to_apply = false;
			finding.reason_code = "font_state_preservation_unproven";
		}
	}
	return plan;
}

FontVariantAuditApplyResult ApplyFontVariantAuditChanges(
	AssFile& file,
	FontVariantAuditPlan const& plan,
	std::span<std::size_t const> selected_finding_indices,
	FontVariantAuditOptions options,
	FontVariantAuditProfileProvider const& profile_provider) {
	FontVariantAuditApplyResult result;
	auto const cached_provider = CachedProfiles(profile_provider);
	std::unordered_set<std::size_t> seen_findings;
	std::unordered_set<std::size_t> seen_styles;

	struct PendingEventFix {
		std::size_t block_index = 0;
		std::size_t font_state_revision = 0;
		FontVariantAuditFinding const* finding = nullptr;
	};
	struct PendingEvent {
		AssDialogue* event = nullptr;
		std::vector<std::unique_ptr<AssDialogueBlock>> blocks;
		std::vector<PendingEventFix> fixes;
	};
	std::map<std::size_t, PendingEvent> pending_events;
	std::vector<FontVariantAuditFinding const*> pending_styles;

	seen_findings.reserve(selected_finding_indices.size());
	seen_styles.reserve(selected_finding_indices.size());
	pending_styles.reserve(selected_finding_indices.size());

	for (auto index : selected_finding_indices) {
		if (index >= plan.findings.size()) {
			result.error = "font variant audit selection is out of range";
			return result;
		}
		if (!seen_findings.insert(index).second) {
			result.error = "font variant audit selection contains a duplicate finding";
			return result;
		}

		auto const& finding = plan.findings[index];
		if (!IsApplicable(finding, options)) {
			result.error = "font variant audit selection contains a report-only finding";
			return result;
		}
		if (finding.snapshot_index >= plan.snapshots.size()) {
			result.error = "font variant audit source snapshot is missing";
			return result;
		}
		auto const& snapshot = plan.snapshots[finding.snapshot_index];
		if (snapshot.kind != finding.source.kind ||
		    snapshot.entry_index != finding.source.entry_index ||
		    !SnapshotMatches(file, snapshot)) {
			result.error = "font variant audit source changed after the plan was built";
			return result;
		}
		if (!LivePinStillMatches(finding, cached_provider)) {
			result.error = "font variant audit live profile changed after the plan was built";
			return result;
		}

		if (finding.source.kind == FontVariantAuditSourceKind::Style) {
			if (!seen_styles.insert(finding.source.entry_index).second) {
				result.error = "font variant audit selection contains conflicting style fixes";
				return result;
			}
			pending_styles.push_back(&finding);
			continue;
		}

		auto* event = EventAt(file, finding.source.entry_index);
		if (!event) {
			result.error = "font variant audit event changed after the plan was built";
			return result;
		}
		auto spans = ScanExplicitFontSpans(file, *event, finding.source.entry_index);
		auto span = std::find_if(spans.begin(), spans.end(), [&](auto const& candidate) {
			return RequestMatches(finding, candidate);
		});
		if (span == spans.end()) {
			result.error = "font variant audit span changed after the plan was built";
			return result;
		}

		auto [pending_it, inserted] = pending_events.try_emplace(finding.source.entry_index);
		auto& pending = pending_it->second;
		if (inserted) {
			pending.event = event;
			pending.blocks = event->ParseTags();
		}
		if (finding.source.block_index >= pending.blocks.size() ||
		    pending.blocks[finding.source.block_index]->GetType() != AssBlockType::PLAIN ||
		    std::any_of(pending.fixes.begin(), pending.fixes.end(), [&](auto const& fix) {
				return fix.block_index == finding.source.block_index;
			})) {
			result.error = "font variant audit selection contains conflicting span fixes";
			return result;
		}
		pending.fixes.push_back({
			finding.source.block_index,
			finding.source.font_state_revision,
			&finding});
	}

	// Prepare every dialogue mutation on parsed copies before touching AssFile.
	for (auto& [event_index, pending] : pending_events) {
		(void)event_index;
		// One implicit pin is sufficient for every plain span in the same
		// effective font state. Keep the earliest selected span in each state;
		// a later \b/\i/\fn/\r/\fe/\fs revision deliberately starts a new
		// state and therefore still gets its own repair.
		std::sort(pending.fixes.begin(), pending.fixes.end(), [](auto const& left, auto const& right) {
			if (left.font_state_revision != right.font_state_revision)
				return left.font_state_revision < right.font_state_revision;
			return left.block_index < right.block_index;
		});
		std::vector<PendingEventFix> coalesced;
		coalesced.reserve(pending.fixes.size());
		for (auto const& fix : pending.fixes) {
			if (!coalesced.empty() &&
			    coalesced.back().font_state_revision == fix.font_state_revision)
				continue;
			coalesced.push_back(fix);
		}
		pending.fixes.swap(coalesced);
		std::sort(pending.fixes.begin(), pending.fixes.end(), [](auto const& left, auto const& right) {
			return left.block_index > right.block_index;
		});
		for (auto const& fix : pending.fixes) {
			auto const& finding = *fix.finding;
			ReplacedExplicitVariants replaced;
			if (finding.has_explicit_bold || finding.has_explicit_italic)
				replaced = ReplaceExplicitVariantTags(pending.blocks, finding);
			auto tags = FixTags(
				finding,
				!replaced.bold,
				!replaced.italic);
			if (!tags.empty()) {
				auto block = std::make_unique<AssDialogueBlockOverride>(tags);
				block->ParseTags();
				pending.blocks.insert(
					pending.blocks.begin() + static_cast<std::ptrdiff_t>(fix.block_index),
					std::move(block));
			}
		}
	}

	AssFile staged;
	CloneFontAuditEntries(file, staged);
	for (auto const *finding : pending_styles) {
		auto *style = StyleAt(staged, finding->source.entry_index);
		if (finding->pin_bold)
			style->bold = true;
		if (finding->pin_italic)
			style->italic = true;
		style->UpdateData();
	}
	for (auto& [event_index, pending] : pending_events)
		EventAt(staged, event_index)->UpdateText(pending.blocks);
	if (!PreservesFontSelections(file, staged, plan.catalog_snapshot, cached_provider)) {
		result.error = "font variant audit cannot preserve every affected font selection";
		return result;
	}

	for (auto const* finding : pending_styles) {
		auto* style = StyleAt(file, finding->source.entry_index);
		if (finding->pin_bold)
			style->bold = true;
		if (finding->pin_italic)
			style->italic = true;
		style->UpdateData();
		result.styles_changed = true;
	}
	for (auto& [event_index, pending] : pending_events) {
		(void)event_index;
		pending.event->UpdateText(pending.blocks);
		result.dialogue_text_changed = true;
	}

	result.applied_change_count = selected_finding_indices.size();
	result.success = true;
	return result;
}
