// Copyright (c) 2026, MIRIMIRIM

#include <aegisub/fontcollector/fontcollector.h>

#include "ass_file.h"
#include "ass_style.h"
#include "ass_io_core.h"
#include "ass_dialogue.h"
#include "font_collector_core.h"
#include "font_family_catalog.h"
#include "font_file_lister.h"
#include "font_name_normalization.h"
#include "text_file_reader.h"

#include <libaegisub/charset.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
static_assert(static_cast<int>(FontCollectionMode::CheckFontsOnly) == AEGISUB_FONTCOLLECTOR_MODE_CHECK);
static_assert(static_cast<int>(FontCollectionMode::CopyToFolder) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER);
static_assert(static_cast<int>(FontCollectionMode::CopyToScriptFolder) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER);
static_assert(static_cast<int>(FontCollectorMatcher::Platform) == AEGISUB_FONTCOLLECTOR_MATCHER_PLATFORM);
static_assert(static_cast<int>(FontCollectorMatcher::Libass) == AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS);
static_assert(AEGISUB_FONTCOLLECTOR_MATCH_MEMORY_ONLY == 2);
static_assert(static_cast<int>(FontCollectorEventType::FontBackendInfo) == AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO);
static_assert(static_cast<int>(FontCollectorEventType::CollectionNewline) == AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE);
static_assert(static_cast<int>(FontNameNormalizationTarget::Localized) == AEGISUB_FONT_NAME_TARGET_LOCALIZED);
static_assert(static_cast<int>(FontNameNormalizationTarget::EnglishWin32) == AEGISUB_FONT_NAME_TARGET_ENGLISH_WIN32);
static_assert(static_cast<int>(FontNameSourceKind::Style) == AEGISUB_FONT_NAME_SOURCE_STYLE);
static_assert(static_cast<int>(FontNameSourceKind::Override) == AEGISUB_FONT_NAME_SOURCE_OVERRIDE);
static_assert(static_cast<int>(FontFamilyMatchKind::None) == AEGISUB_FONT_FAMILY_MATCH_NONE);
static_assert(static_cast<int>(FontFamilyMatchKind::Ambiguous) == AEGISUB_FONT_FAMILY_MATCH_AMBIGUOUS);
static_assert(static_cast<int>(FontVariantRole::Unknown) == AEGISUB_FONTCOLLECTOR_VARIANT_UNKNOWN);
static_assert(static_cast<int>(FontVariantRole::Regular) == AEGISUB_FONTCOLLECTOR_VARIANT_REGULAR);
static_assert(static_cast<int>(FontVariantRole::Bold) == AEGISUB_FONTCOLLECTOR_VARIANT_BOLD);
static_assert(static_cast<int>(FontVariantRole::Italic) == AEGISUB_FONTCOLLECTOR_VARIANT_ITALIC);
static_assert(static_cast<int>(FontVariantRole::BoldItalic) == AEGISUB_FONTCOLLECTOR_VARIANT_BOLD_ITALIC);
static_assert(static_cast<int>(FontVariantStatus::Unknown) == AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_UNKNOWN);
static_assert(static_cast<int>(FontVariantStatus::Canonical) == AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_CANONICAL);
static_assert(static_cast<int>(FontVariantStatus::NonCanonical) == AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_NONCANONICAL);
static_assert(static_cast<int>(FontVariantStatus::Synthetic) == AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_SYNTHETIC);
static_assert(AEGISUB_FONT_NAME_NORMALIZATION_REQUEST_V1_SIZE <= sizeof(AegisubFontNameNormalizationRequest));
static_assert(AEGISUB_FONT_NAME_NORMALIZATION_CHANGE_V1_SIZE <= sizeof(AegisubFontNameNormalizationChange));
static_assert(AEGISUB_FONT_NAME_NORMALIZATION_SUMMARY_V1_SIZE <= sizeof(AegisubFontNameNormalizationSummary));

AegisubFontCollectorVariantRole ToCVariantRole(std::optional<FontVariantRole> role) {
	if (!role)
		return AEGISUB_FONTCOLLECTOR_VARIANT_UNKNOWN;
	switch (*role) {
		case FontVariantRole::Regular: return AEGISUB_FONTCOLLECTOR_VARIANT_REGULAR;
		case FontVariantRole::Bold: return AEGISUB_FONTCOLLECTOR_VARIANT_BOLD;
		case FontVariantRole::Italic: return AEGISUB_FONTCOLLECTOR_VARIANT_ITALIC;
		case FontVariantRole::BoldItalic: return AEGISUB_FONTCOLLECTOR_VARIANT_BOLD_ITALIC;
		case FontVariantRole::Unknown: return AEGISUB_FONTCOLLECTOR_VARIANT_UNKNOWN;
	}
	return AEGISUB_FONTCOLLECTOR_VARIANT_UNKNOWN;
}

AegisubFontCollectorVariantStatus ToCVariantStatus(std::optional<FontVariantStatus> status) {
	if (!status)
		return AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_UNKNOWN;
	switch (*status) {
		case FontVariantStatus::Canonical: return AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_CANONICAL;
		case FontVariantStatus::NonCanonical: return AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_NONCANONICAL;
		case FontVariantStatus::Synthetic: return AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_SYNTHETIC;
		case FontVariantStatus::Unknown: return AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_UNKNOWN;
	}
	return AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_UNKNOWN;
}
static_assert(AEGISUB_FONT_NAME_NORMALIZATION_BATCH_ITEM_V1_SIZE <= sizeof(AegisubFontNameNormalizationBatchItem));

void WriteError(char *buffer, size_t buffer_size, std::string const& message) {
	if (!buffer || buffer_size == 0)
		return;

	std::snprintf(buffer, buffer_size, "%s", message.c_str());
}

bool StartsWith(std::string_view value, std::string_view prefix) {
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool IsEventLine(std::string_view line) {
	return StartsWith(line, "Dialogue:") || StartsWith(line, "Comment:");
}

std::string_view TrimAsciiWhitespace(std::string_view line) {
	while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
		line.remove_prefix(1);
	while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
		line.remove_suffix(1);
	return line;
}

bool IsSectionHeader(std::string_view line) {
	return line.size() >= 2 && line.front() == '[' && line.back() == ']';
}

enum class SourceSection {
	Other,
	Styles,
	Events
};

std::string LowerAscii(std::string_view value) {
	std::string result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return result;
}

std::vector<int> AssignSourceLineNumbers(
	AssFile& subs,
	agi::fs::path const& input_path,
	std::string const& encoding)
{
	TextFileReader reader(input_path, encoding);
	auto style = subs.Styles.begin();
	auto event = subs.Events.begin();
	SourceSection section = SourceSection::Other;
	std::vector<int> style_lines;
	style_lines.reserve(subs.Styles.size());
	for (int line_number = 1; reader.HasMoreLines(); ++line_number) {
		auto line = reader.ReadLineFromFile();
		auto const trimmed_line = TrimAsciiWhitespace(line);
		if (IsSectionHeader(trimmed_line)) {
			auto const header = LowerAscii(trimmed_line);
			if (header == "[v4 styles]" || header == "[v4+ styles]")
				section = SourceSection::Styles;
			else if (header == "[events]")
				section = SourceSection::Events;
			else
				section = SourceSection::Other;
			continue;
		}

		if (section == SourceSection::Styles && StartsWith(line, "Style:")) {
			if (style != subs.Styles.end()) {
				style_lines.push_back(line_number);
				++style;
			}
			continue;
		}

		if (section == SourceSection::Events && IsEventLine(line) && event != subs.Events.end()) {
			event->Row = line_number - 1;
			++event;
		}
	}
	return style_lines;
}

bool IsNormalizationTargetValid(AegisubFontNameNormalizationTarget target) {
	return target == AEGISUB_FONT_NAME_TARGET_LOCALIZED ||
	       target == AEGISUB_FONT_NAME_TARGET_ENGLISH_WIN32;
}

int ValidateNormalizationRequest(
	AegisubFontNameNormalizationRequest const *request,
	char *error_buffer,
	size_t error_buffer_size)
{
	if (!request || request->struct_size < AEGISUB_FONT_NAME_NORMALIZATION_REQUEST_V1_SIZE) {
		WriteError(error_buffer, error_buffer_size, "normalization request with a valid struct_size is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (!request->input_path || !*request->input_path) {
		WriteError(error_buffer, error_buffer_size, "input_path is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (!IsNormalizationTargetValid(request->target)) {
		WriteError(error_buffer, error_buffer_size, "invalid font-name normalization target");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	return AEGISUB_FONTCOLLECTOR_OK;
}

int ValidateNormalizationSummary(
	AegisubFontNameNormalizationSummary const *summary,
	char *error_buffer,
	size_t error_buffer_size)
{
	if (summary && summary->struct_size < AEGISUB_FONT_NAME_NORMALIZATION_SUMMARY_V1_SIZE) {
		WriteError(error_buffer, error_buffer_size, "normalization summary struct_size is too small");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	return AEGISUB_FONTCOLLECTOR_OK;
}

void ResetNormalizationSummary(AegisubFontNameNormalizationSummary *summary) {
	if (!summary)
		return;
	auto const capacity = summary->struct_size;
	std::memset(summary, 0, std::min(capacity, sizeof(*summary)));
	summary->struct_size = capacity;
}

void WriteNormalizationSummary(
	AegisubFontNameNormalizationSummary *destination,
	AegisubFontNameNormalizationSummary const& source)
{
	if (!destination)
		return;
	auto const capacity = destination->struct_size;
	std::memcpy(destination, &source, std::min(capacity, sizeof(source)));
	destination->struct_size = capacity;
}

void EmitNormalizationChange(
	FontNameNormalizationChange const& change,
	AegisubFontNameNormalizationCallback callback,
	void *user_data)
{
	if (!callback)
		return;

	AegisubFontNameNormalizationChange c_change = {};
	c_change.struct_size = sizeof(c_change);
	c_change.source_kind = static_cast<AegisubFontNameSourceKind>(change.source.kind);
	c_change.style = change.source.style.c_str();
	c_change.line = change.source.line;
	c_change.override_index = change.source.override_index;
	c_change.comment = change.source.comment;
	c_change.current_name = change.current_name.c_str();
	c_change.recommended_name = change.recommended_name.c_str();
	c_change.match_kind = static_cast<AegisubFontFamilyMatchKind>(change.match_kind);
	c_change.reason_code = change.reason_code.c_str();
	c_change.safe_to_apply = change.safe_to_apply;
	callback(&c_change, user_data);
}

int BuildNormalizationPlanWithCatalog(
	FontFamilyCatalog const& catalog,
	AegisubFontNameNormalizationRequest const *request,
	AegisubFontNameNormalizationCallback callback,
	void *user_data,
	AegisubFontNameNormalizationSummary *summary,
	char *error_buffer,
	size_t error_buffer_size)
{
	WriteError(error_buffer, error_buffer_size, "");
	if (auto result = ValidateNormalizationSummary(summary, error_buffer, error_buffer_size))
		return result;
	ResetNormalizationSummary(summary);
	if (auto result = ValidateNormalizationRequest(request, error_buffer, error_buffer_size))
		return result;

	try {
		auto input_path = agi::fs::PathFromString(request->input_path);
		auto encoding = request->encoding && *request->encoding
			? std::string(request->encoding)
			: agi::charset::Detect(input_path);
		auto subs = ReadAssFileForCore(input_path, encoding);
		auto style_source_lines = AssignSourceLineNumbers(subs, input_path, encoding);
		auto plan = BuildFontNameNormalizationPlan(
			subs,
			catalog,
			static_cast<FontNameNormalizationTarget>(request->target),
			style_source_lines);

		AegisubFontNameNormalizationSummary local_summary = {};
		local_summary.struct_size = sizeof(local_summary);
		local_summary.catalog_available = plan.catalog_available;
		local_summary.scanned_name_count = plan.scanned_name_count;
		local_summary.finding_count = plan.changes.size();
		for (auto const& change : plan.changes) {
			if (change.safe_to_apply)
				++local_summary.safe_change_count;
			else
				++local_summary.unsafe_finding_count;
			EmitNormalizationChange(change, callback, user_data);
		}
		WriteNormalizationSummary(summary, local_summary);
		return AEGISUB_FONTCOLLECTOR_OK;
	}
	catch (agi::Exception const& e) {
		WriteError(error_buffer, error_buffer_size, e.GetMessage());
	}
	catch (std::exception const& e) {
		WriteError(error_buffer, error_buffer_size, e.what());
	}
	catch (...) {
		WriteError(error_buffer, error_buffer_size, "unknown error");
	}
	return AEGISUB_FONTCOLLECTOR_READ_FAILED;
}

void ResetSummary(AegisubFontCollectorSummary *summary) {
	if (summary)
		*summary = {};
}

void AccumulateSummary(AegisubFontCollectorSummary& summary, FontCollectorEvent const& event) {
	switch (event.type) {
		case FontCollectorEventType::StyleMissing:
			++summary.missing_style_count;
			break;
		case FontCollectorEventType::FontFound:
			++summary.found_font_count;
			break;
		case FontCollectorEventType::FontMissing:
			++summary.missing_font_count;
			break;
		case FontCollectorEventType::MissingGlyphs:
			++summary.missing_glyph_font_count;
			break;
		case FontCollectorEventType::FakeBold:
			++summary.fake_bold_count;
			break;
		case FontCollectorEventType::FakeItalic:
			++summary.fake_italic_count;
			break;
		case FontCollectorEventType::CollectionCopied:
		case FontCollectorEventType::CollectionAlreadyExists:
			++summary.copied_font_count;
			break;
		case FontCollectorEventType::CollectionFailedCreateDirectory:
		case FontCollectorEventType::CollectionFailedOpen:
		case FontCollectorEventType::CollectionFailedCopy:
			++summary.collection_failure_count;
			break;
		default:
			break;
	}
}

bool ToCoreMode(AegisubFontCollectorMode mode, FontCollectionMode& out) {
	switch (mode) {
		case AEGISUB_FONTCOLLECTOR_MODE_CHECK:
			out = FontCollectionMode::CheckFontsOnly;
			return true;
		case AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER:
			out = FontCollectionMode::CopyToFolder;
			return true;
		case AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER:
			out = FontCollectionMode::CopyToScriptFolder;
			return true;
	}
	return false;
}

bool ToCoreMatcher(AegisubFontCollectorMatcher matcher, FontCollectorMatcher& out) {
	switch (matcher) {
		case AEGISUB_FONTCOLLECTOR_MATCHER_PLATFORM:
			out = FontCollectorMatcher::Platform;
			return true;
		case AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS:
			out = FontCollectorMatcher::Libass;
			return true;
	}
	return false;
}

int DestinationError(FontCollectionDestinationResult result, char *error_buffer, size_t error_buffer_size) {
	if (result.invalid_destination) {
		WriteError(error_buffer, error_buffer_size, "destination path is an existing file");
		return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
	}

	switch (result.error) {
		case FontCollectionDestinationError::None:
			return AEGISUB_FONTCOLLECTOR_OK;
		case FontCollectionDestinationError::CouldNotCreateDestinationFolder:
			WriteError(error_buffer, error_buffer_size, "could not create destination folder");
			return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
		case FontCollectionDestinationError::InvalidArchivePath:
			WriteError(error_buffer, error_buffer_size, "zip archive destination is not valid");
			return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
	}

	WriteError(error_buffer, error_buffer_size, "unknown destination error");
	return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
}

void EmitCEvent(FontCollectorEvent const& event, AegisubFontCollectorEventCallback callback, void *user_data) {
	if (!callback)
		return;

	std::string path_text;
	if (!event.path.empty())
		path_text = agi::fs::PathToString(event.path);

	std::vector<char const*> styles;
	styles.reserve(event.styles.size());
	for (auto const& style : event.styles)
		styles.push_back(style.c_str());

	AegisubFontCollectorEvent c_event = {};
	c_event.type = static_cast<AegisubFontCollectorEventType>(event.type);
	c_event.face = event.face.c_str();
	c_event.message = event.message.c_str();
	c_event.style = event.style.c_str();
	c_event.path = path_text.c_str();
	c_event.styles = styles.empty() ? nullptr : styles.data();
	c_event.style_count = styles.size();
	c_event.lines = event.lines.empty() ? nullptr : event.lines.data();
	c_event.line_count = event.lines.size();
	c_event.count = event.count;
	c_event.requested_weight = event.requested_weight;
	c_event.requested_italic = event.requested_italic;

	callback(&c_event, user_data);
}

void EmitCUsage(FontCollectorAssFontUsage const& usage,
                AegisubFontCollectorFontUsageCallback callback,
                void *user_data) {
	if (!callback)
		return;

	std::vector<std::string> path_texts;
	path_texts.reserve(usage.matched.paths.size());
	std::vector<char const*> paths;
	paths.reserve(usage.matched.paths.size());
	for (auto const& path : usage.matched.paths) {
		path_texts.push_back(agi::fs::PathToString(path));
		paths.push_back(path_texts.back().c_str());
	}

	std::vector<char const*> matched_names;
	matched_names.reserve(usage.matched.names.size());
	for (auto const& name : usage.matched.names)
		matched_names.push_back(name.c_str());

	std::vector<char const*> styles;
	styles.reserve(usage.styles.size());
	for (auto const& style : usage.styles)
		styles.push_back(style.c_str());

	std::vector<AegisubFontCollectorMatchCandidate> match_candidates;
	match_candidates.reserve(usage.matched.match_candidates.size());
	for (auto const& source : usage.matched.match_candidates) {
		AegisubFontCollectorMatchCandidate candidate = {};
		candidate.facename = source.facename.c_str();
		candidate.facename_full = source.facename_full.c_str();
		candidate.matched_name = source.matched_name.c_str();
		candidate.match_source = source.match_source.c_str();
		candidate.name_match = source.name_match.c_str();
		candidate.path = source.path.c_str();
		candidate.provider_order = source.provider_order;
		candidate.face_index = source.face_index;
		candidate.score = source.score;
		candidate.weight = source.weight;
		candidate.bold = source.bold;
		candidate.italic = source.italic;
		candidate.considered_codepoints = source.considered_codepoints.empty() ? nullptr : source.considered_codepoints.data();
		candidate.considered_codepoint_count = source.considered_codepoints.size();
		candidate.supported_codepoints = source.supported_codepoints.empty() ? nullptr : source.supported_codepoints.data();
		candidate.supported_codepoint_count = source.supported_codepoints.size();
		candidate.selected_codepoints = source.selected_codepoints.empty() ? nullptr : source.selected_codepoints.data();
		candidate.selected_codepoint_count = source.selected_codepoints.size();
		match_candidates.push_back(candidate);
	}

	AegisubFontCollectorFontUsage c_usage = {};
	c_usage.ass_facename = usage.ass_facename.c_str();
	c_usage.ass_bold = usage.ass_bold;
	c_usage.ass_italic = usage.ass_italic;
	c_usage.codepoints = usage.codepoints.empty() ? nullptr : usage.codepoints.data();
	c_usage.codepoint_count = usage.codepoints.size();
	c_usage.styles = styles.empty() ? nullptr : styles.data();
	c_usage.style_count = styles.size();
	c_usage.lines = usage.lines.empty() ? nullptr : usage.lines.data();
	c_usage.line_count = usage.lines.size();
	c_usage.override_lines = usage.override_lines.empty() ? nullptr : usage.override_lines.data();
	c_usage.override_line_count = usage.override_lines.size();
	if (usage.matched.paths.empty() && usage.matched.memory_fonts.empty())
		c_usage.matched.match_status = AEGISUB_FONTCOLLECTOR_MATCH_MISSING;
	else if (usage.matched.paths.empty())
		c_usage.matched.match_status = AEGISUB_FONTCOLLECTOR_MATCH_MEMORY_ONLY;
	else
		c_usage.matched.match_status = AEGISUB_FONTCOLLECTOR_MATCH_FOUND;
	c_usage.matched.facename = usage.matched.facename.c_str();
	c_usage.matched.face_index = usage.matched.face_index;
	c_usage.matched.weight = usage.matched.weight;
	c_usage.matched.bold = usage.matched.bold;
	c_usage.matched.italic = usage.matched.italic;
	c_usage.matched.path_source = usage.matched.path_source.c_str();
	c_usage.matched.paths = paths.empty() ? nullptr : paths.data();
	c_usage.matched.path_count = paths.size();
	c_usage.matched.fake_bold = usage.matched.fake_bold;
	c_usage.matched.fake_italic = usage.matched.fake_italic;
	c_usage.matched.missing_text = usage.matched.missing_text.c_str();
	c_usage.matched.missing_codepoints = usage.matched.missing_codepoints.empty() ? nullptr : usage.matched.missing_codepoints.data();
	c_usage.matched.missing_codepoint_count = usage.matched.missing_codepoints.size();
	c_usage.matched.requested_weight = usage.matched.requested_weight;
	c_usage.matched.missing_lines = usage.matched.missing_lines.empty() ? nullptr : usage.matched.missing_lines.data();
	c_usage.matched.missing_line_count = usage.matched.missing_lines.size();
	c_usage.matched_facename_full = usage.matched.facename_full.c_str();
	c_usage.matched_names = matched_names.empty() ? nullptr : matched_names.data();
	c_usage.matched_name_count = matched_names.size();
	c_usage.match_candidates = match_candidates.empty() ? nullptr : match_candidates.data();
	c_usage.match_candidate_count = match_candidates.size();
	c_usage.match_ambiguous = usage.matched.match_ambiguous;
	c_usage.ass_effective_weight = usage.ass_effective_weight;
	c_usage.ass_charset = usage.ass_charset;
	c_usage.ass_height = usage.ass_height;
	c_usage.ass_raw_bold_tag = usage.ass_raw_bold_tag.c_str();
	c_usage.ass_raw_italic_tag = usage.ass_raw_italic_tag.c_str();
	c_usage.ass_raw_charset_tag = usage.ass_raw_charset_tag.c_str();
	c_usage.ass_raw_height_tag = usage.ass_raw_height_tag.c_str();
	c_usage.ass_has_explicit_family = usage.ass_has_explicit_family;
	c_usage.ass_has_explicit_bold = usage.ass_has_explicit_bold;
	c_usage.ass_has_explicit_italic = usage.ass_has_explicit_italic;
	c_usage.ass_has_explicit_charset = usage.ass_has_explicit_charset;
	c_usage.ass_has_explicit_height = usage.ass_has_explicit_height;
	c_usage.baseline_facename = usage.baseline_facename.c_str();
	c_usage.baseline_weight = usage.baseline_weight;
	c_usage.baseline_italic = usage.baseline_italic;
	c_usage.baseline_charset = usage.baseline_charset;
	c_usage.baseline_height = usage.baseline_height;
	c_usage.matched_has_backend_requested_weight = usage.matched.backend_requested_weight.has_value();
	c_usage.matched_backend_requested_weight = usage.matched.backend_requested_weight.value_or(0);
	c_usage.matched_realized_role = ToCVariantRole(usage.matched.realized_role);
	c_usage.matched_realized_status = ToCVariantStatus(usage.matched.realized_status);
	c_usage.matched_implicit_variant_fallback = usage.matched.implicit_variant_fallback;
	c_usage.matched_noncanonical_variant = usage.matched.noncanonical_variant;

	callback(&c_usage, user_data);
}

struct BatchLoadedItem {
	AegisubFontCollectorBatchItem *item = nullptr;
	FontCollectionMode mode = FontCollectionMode::CheckFontsOnly;
	agi::fs::path destination;
	std::unique_ptr<AssFile> subs;
	FontCollectorDetails details;
	AegisubFontCollectorSummary summary = {};
};

void SetBatchItemResult(AegisubFontCollectorBatchItem& item, int result, std::string const& error = {}) {
	item.result = result;
	WriteError(item.error_buffer, item.error_buffer_size, error);
	ResetSummary(item.summary);
}

void FinishBatchItem(BatchLoadedItem& loaded) {
	auto& item = *loaded.item;
	loaded.summary.font_usage_count = loaded.details.fonts.size();
	if (item.summary)
		*item.summary = loaded.summary;

	for (auto const& usage : loaded.details.fonts)
		EmitCUsage(usage, item.usage_callback, item.usage_user_data);

	item.result = AEGISUB_FONTCOLLECTOR_OK;
}

}

struct AegisubFontCollectorSession {
	FontCollectorSession core;

	AegisubFontCollectorSession(FontCollectorMatcher core_matcher,
	                            FontCollectorEventSink event_sink,
	                            FontProviderOptions provider_options = {})
	: core(std::move(event_sink), core_matcher, std::move(provider_options))
	{
	}
};

extern "C" int aegisub_fontcollector_session_create_with_options(
	AegisubFontCollectorSessionOptions const *options,
	AegisubFontCollectorEventCallback callback,
	void *user_data,
	AegisubFontCollectorSession **session,
	char *error_buffer,
	size_t error_buffer_size) {
	WriteError(error_buffer, error_buffer_size, "");
	if (!options || options->struct_size < AEGISUB_FONTCOLLECTOR_SESSION_OPTIONS_V1_SIZE) {
		WriteError(error_buffer, error_buffer_size, "valid session options are required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (!session) {
		WriteError(error_buffer, error_buffer_size, "session output pointer is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	*session = nullptr;

	FontCollectorMatcher core_matcher;
	if (!ToCoreMatcher(options->matcher, core_matcher)) {
		WriteError(error_buffer, error_buffer_size, "invalid font matcher");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (options->additional_font_file_count && !options->additional_font_files) {
		WriteError(error_buffer, error_buffer_size, "additional font file array is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (options->matcher != AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS &&
	    (options->additional_font_file_count || !options->include_system_fonts)) {
		WriteError(error_buffer, error_buffer_size, "private font options require the libass matcher");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (options->matcher == AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS &&
	    !options->include_system_fonts &&
	    options->additional_font_file_count == 0) {
		WriteError(error_buffer, error_buffer_size,
		           "libass private catalog requires additional font files (include_system_fonts is 0; set it to 1 or supply additional_font_files)");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	FontProviderOptions provider_options;
	provider_options.include_system_fonts = options->include_system_fonts != 0;
	provider_options.collect_match_candidates = options->collect_match_candidates != 0;
	provider_options.additional_font_files.reserve(options->additional_font_file_count);
	for (size_t i = 0; i < options->additional_font_file_count; ++i) {
		auto path = options->additional_font_files[i];
		if (!path || !*path) {
			WriteError(error_buffer, error_buffer_size, "additional font file path must not be empty");
			return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
		}
		provider_options.additional_font_files.emplace_back(path);
	}

	try {
		auto event_sink = [callback, user_data](FontCollectorEvent const& event) {
			EmitCEvent(event, callback, user_data);
		};
		auto handle = std::make_unique<AegisubFontCollectorSession>(
			core_matcher,
			std::move(event_sink),
			std::move(provider_options));
		*session = handle.release();
		return AEGISUB_FONTCOLLECTOR_OK;
	}
	catch (std::exception const& e) {
		WriteError(error_buffer, error_buffer_size, e.what());
		return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}
	catch (...) {
		WriteError(error_buffer, error_buffer_size, "unknown error");
		return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}
}

extern "C" int aegisub_fontcollector_session_collect_batch(
	AegisubFontCollectorSession *session,
	AegisubFontCollectorBatchItem *items,
	size_t item_count,
	char *error_buffer,
	size_t error_buffer_size) {
	WriteError(error_buffer, error_buffer_size, "");

	if (!session) {
		WriteError(error_buffer, error_buffer_size, "session is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (!items && item_count) {
		WriteError(error_buffer, error_buffer_size, "batch items are required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	std::vector<BatchLoadedItem> loaded_items;
	loaded_items.reserve(item_count);
	bool have_batch_mode = false;
	FontCollectionMode batch_mode = FontCollectionMode::CheckFontsOnly;

	for (size_t i = 0; i < item_count; ++i) {
		auto& item = items[i];
		item.result = AEGISUB_FONTCOLLECTOR_OK;
		WriteError(item.error_buffer, item.error_buffer_size, "");
		ResetSummary(item.summary);

		auto const& request = item.request;
		if (!request.input_path || !*request.input_path) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT, "input_path is required");
			continue;
		}

		FontCollectionMode mode;
		if (!ToCoreMode(request.mode, mode)) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_MODE, "invalid collection mode");
			continue;
		}

		if (!have_batch_mode) {
			batch_mode = mode;
			have_batch_mode = true;
		}
		else if (mode != batch_mode) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_MODE, "all batch items must use the same collection mode");
			continue;
		}

		try {
			auto input_path = agi::fs::PathFromString(request.input_path);
			auto destination = request.destination_path && *request.destination_path
				? agi::fs::PathFromString(request.destination_path)
				: agi::fs::path();

			if (mode == FontCollectionMode::CopyToScriptFolder) {
				destination = input_path.parent_path();
				if (destination.empty())
					destination = std::filesystem::current_path();
			}

			if (mode == FontCollectionMode::CopyToFolder && destination.empty()) {
				SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT, "destination_path is required for this collection mode");
				continue;
			}

			auto destination_result = PrepareFontCollectionDestination(mode, destination);
			if (auto destination_error = DestinationError(destination_result, item.error_buffer, item.error_buffer_size)) {
				ResetSummary(item.summary);
				item.result = destination_error;
				continue;
			}

			auto encoding = request.encoding && *request.encoding
				? std::string(request.encoding)
				: agi::charset::Detect(input_path);

			BatchLoadedItem loaded;
			loaded.item = &item;
			loaded.mode = mode;
			loaded.destination = destination;
			loaded.subs = std::make_unique<AssFile>(ReadAssFileForCore(input_path, encoding));
			AssignSourceLineNumbers(*loaded.subs, input_path, encoding);
			loaded_items.push_back(std::move(loaded));
		}
		catch (agi::Exception const& e) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.GetMessage());
		}
		catch (std::exception const& e) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.what());
		}
		catch (...) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, "unknown error");
		}
	}

	if (loaded_items.empty())
		return AEGISUB_FONTCOLLECTOR_OK;

	std::vector<FontCollectionBatchSource> sources;
	sources.reserve(loaded_items.size());
	for (auto& loaded : loaded_items) {
		auto *loaded_ptr = &loaded;
		FontCollectionBatchSource source;
		source.subs = loaded_ptr->subs.get();
		source.destination = loaded_ptr->destination;
		source.details = (loaded_ptr->item->usage_callback || loaded_ptr->item->summary) ? &loaded_ptr->details : nullptr;
		source.font_event_sink = [loaded_ptr](FontCollectorEvent const& event) {
			AccumulateSummary(loaded_ptr->summary, event);
			EmitCEvent(event, loaded_ptr->item->event_callback, loaded_ptr->item->event_user_data);
		};
		sources.push_back(std::move(source));
	}

	try {
		CollectFonts(
			session->core,
			sources,
			batch_mode,
			{});

		for (auto& loaded : loaded_items)
			FinishBatchItem(loaded);
	}
	catch (agi::Exception const& e) {
		for (auto& loaded : loaded_items)
			SetBatchItemResult(*loaded.item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.GetMessage());
	}
	catch (std::exception const& e) {
		for (auto& loaded : loaded_items)
			SetBatchItemResult(*loaded.item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.what());
	}
	catch (...) {
		for (auto& loaded : loaded_items)
			SetBatchItemResult(*loaded.item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, "unknown error");
	}

	return AEGISUB_FONTCOLLECTOR_OK;
}

extern "C" void aegisub_fontcollector_session_destroy(AegisubFontCollectorSession *session) {
	delete session;
}

extern "C" int aegisub_fontcollector_build_normalization_plan(
	AegisubFontNameNormalizationRequest const *request,
	AegisubFontNameNormalizationCallback callback,
	void *user_data,
	AegisubFontNameNormalizationSummary *summary,
	char *error_buffer,
	size_t error_buffer_size)
{
	WriteError(error_buffer, error_buffer_size, "");
	if (auto result = ValidateNormalizationSummary(summary, error_buffer, error_buffer_size))
		return result;
	ResetNormalizationSummary(summary);
	if (auto result = ValidateNormalizationRequest(request, error_buffer, error_buffer_size))
		return result;

	try {
		auto catalog = BuildFontFamilyCatalog();
		return BuildNormalizationPlanWithCatalog(
			catalog, request, callback, user_data, summary, error_buffer, error_buffer_size);
	}
	catch (std::exception const& e) {
		WriteError(error_buffer, error_buffer_size, e.what());
	}
	catch (...) {
		WriteError(error_buffer, error_buffer_size, "failed to build font-family catalog");
	}
	return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
}

extern "C" int aegisub_fontcollector_build_normalization_plan_batch(
	AegisubFontNameNormalizationBatchItem *const *items,
	size_t item_count,
	char *error_buffer,
	size_t error_buffer_size)
{
	WriteError(error_buffer, error_buffer_size, "");
	if (!items && item_count) {
		WriteError(error_buffer, error_buffer_size, "normalization batch items are required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	for (size_t i = 0; i < item_count; ++i) {
		if (!items[i]) {
			WriteError(error_buffer, error_buffer_size, "normalization batch item is required");
			return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
		}
		if (items[i]->struct_size < AEGISUB_FONT_NAME_NORMALIZATION_BATCH_ITEM_V1_SIZE) {
			WriteError(error_buffer, error_buffer_size, "normalization batch item struct_size is too small");
			return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
		}
		items[i]->result = AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}

	FontFamilyCatalog catalog;
	try {
		catalog = BuildFontFamilyCatalog();
	}
	catch (std::exception const& e) {
		WriteError(error_buffer, error_buffer_size, e.what());
		return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}
	catch (...) {
		WriteError(error_buffer, error_buffer_size, "failed to build font-family catalog");
		return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}

	for (size_t i = 0; i < item_count; ++i) {
		auto& item = *items[i];
		item.result = BuildNormalizationPlanWithCatalog(
			catalog,
			item.request,
			item.callback,
			item.user_data,
			item.summary,
			item.error_buffer,
			item.error_buffer_size);
	}
	return AEGISUB_FONTCOLLECTOR_OK;
}
