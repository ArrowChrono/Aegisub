// Copyright (c) 2026, MIRIMIRIM

#include <aegisub/fontcollector/fontcollector.h>

#include "fontcollector_subcommand.h"
#include "fontcollector_cli_encoding.h"
#include "font_usage_inventory.h"

#include <libaegisub/fs.h>
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/writer.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
constexpr int ValidationFailedExitCode = 20;

std::filesystem::path Utf8ToPath(std::string_view value) {
	return agi::fs::PathFromString(std::string(value));
}

std::string PathToUtf8(std::filesystem::path const& path) {
	return agi::fs::PathToString(path);
}

std::vector<std::string> ResolveCliEncodings(
	std::vector<std::string> const& inputs,
	std::string const& explicit_encoding) {
	std::vector<std::string> encodings;
	encodings.reserve(inputs.size());
	for (auto const& input : inputs) {
		encodings.push_back(explicit_encoding.empty()
			? aegisub::fontcollector_cli::PreferredAutomaticEncoding(Utf8ToPath(input))
			: explicit_encoding);
	}
	return encodings;
}

std::string Safe(char const *text) {
	return text ? std::string(text) : std::string();
}

std::string TrimAsciiWhitespace(std::string value) {
	auto is_space = [](unsigned char ch) {
		return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
	};
	auto begin = std::find_if_not(value.begin(), value.end(), [&](char ch) {
		return is_space(static_cast<unsigned char>(ch));
	});
	auto end = std::find_if_not(value.rbegin(), value.rend(), [&](char ch) {
		return is_space(static_cast<unsigned char>(ch));
	}).base();
	if (begin >= end)
		return {};
	return std::string(begin, end);
}

void AddUnique(std::vector<std::string>& values, std::string value) {
	value = TrimAsciiWhitespace(std::move(value));
	if (value.empty())
		return;
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(std::move(value));
}

bool IsSubtitleFile(std::filesystem::path const& path) {
	auto ext = PathToUtf8(path.extension());
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return ext == ".ass" || ext == ".ssa";
}

bool IsFontFile(std::filesystem::path const& path) {
	auto ext = PathToUtf8(path.extension());
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

std::string DedupeKey(std::filesystem::path const& path) {
	std::error_code ec;
	auto canonical = std::filesystem::weakly_canonical(path, ec);
	auto key_path = ec ? std::filesystem::absolute(path, ec) : canonical;
	if (ec)
		key_path = path;

	auto key = PathToUtf8(key_path);
#ifdef _WIN32
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
#endif
	return key;
}

void AddInputFile(std::filesystem::path const& path, std::vector<std::string>& inputs, std::set<std::string>& seen) {
	auto key = DedupeKey(path);
	if (!seen.insert(key).second)
		return;

	std::error_code ec;
	auto absolute = std::filesystem::absolute(path, ec);
	inputs.push_back(PathToUtf8(ec ? path : absolute));
}

std::vector<std::string> ExpandInputs(std::vector<std::string> const& arguments, bool recursive) {
	std::vector<std::string> inputs;
	std::set<std::string> seen;

	for (auto const& argument : arguments) {
		auto path = Utf8ToPath(argument);
		std::error_code ec;
		if (!std::filesystem::is_directory(path, ec)) {
			AddInputFile(path, inputs, seen);
			continue;
		}

		if (recursive) {
			std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec);
			std::filesystem::recursive_directory_iterator end;
			for (; !ec && it != end; ) {
				std::error_code file_ec;
				if (it->is_regular_file(file_ec) && !file_ec && IsSubtitleFile(it->path()))
					AddInputFile(it->path(), inputs, seen);
				it.increment(ec);
				if (ec)
					break;
			}
		}
		else {
			std::filesystem::directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec);
			std::filesystem::directory_iterator end;
			for (; !ec && it != end; ) {
				std::error_code file_ec;
				if (it->is_regular_file(file_ec) && !file_ec && IsSubtitleFile(it->path()))
					AddInputFile(it->path(), inputs, seen);
				it.increment(ec);
				if (ec)
					break;
			}
		}
	}

	return inputs;
}

std::string EventTypeName(AegisubFontCollectorEventType type) {
	switch (type) {
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO: return "font_backend_info";
		case AEGISUB_FONTCOLLECTOR_EVENT_UPDATING_FONT_CACHE: return "updating_font_cache";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR: return "font_cache_error";
		case AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE: return "parsing_file";
		case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING: return "style_missing";
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCHING_FOR_FONT_FILES: return "searching_for_font_files";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING: return "font_missing";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_FOUND: return "font_found";
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_BOLD: return "fake_bold";
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_ITALIC: return "fake_italic";
		case AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS: return "missing_glyphs";
		case AEGISUB_FONTCOLLECTOR_EVENT_USAGE: return "usage";
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCH_COMPLETE: return "search_complete";
		case AEGISUB_FONTCOLLECTOR_EVENT_ALL_FONTS_FOUND: return "all_fonts_found";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING: return "fonts_missing";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING_GLYPHS: return "fonts_missing_glyphs";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_FOLDER: return "collection_copying_fonts_to_folder";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_ARCHIVE: return "collection_copying_fonts_to_archive";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_CREATE_DIRECTORY: return "collection_failed_create_directory";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_OPEN: return "collection_failed_open";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPIED: return "collection_copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_ALREADY_EXISTS: return "collection_already_exists";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_COPY: return "collection_failed_copy";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_ALL_COPIED: return "collection_done_all_copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_SOME_NOT_COPIED: return "collection_done_some_not_copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE: return "collection_newline";
	}
	return "unknown";
}

std::string MatchStatusName(AegisubFontCollectorMatchStatus status) {
	switch (status) {
		case AEGISUB_FONTCOLLECTOR_MATCH_FOUND: return "found";
		case AEGISUB_FONTCOLLECTOR_MATCH_MISSING: return "missing";
		case AEGISUB_FONTCOLLECTOR_MATCH_MEMORY_ONLY: return "memory_only";
	}
	return "unknown";
}

std::string VariantRoleName(AegisubFontCollectorVariantRole role) {
	switch (role) {
		case AEGISUB_FONTCOLLECTOR_VARIANT_REGULAR: return "regular";
		case AEGISUB_FONTCOLLECTOR_VARIANT_BOLD: return "bold";
		case AEGISUB_FONTCOLLECTOR_VARIANT_ITALIC: return "italic";
		case AEGISUB_FONTCOLLECTOR_VARIANT_BOLD_ITALIC: return "bold_italic";
		case AEGISUB_FONTCOLLECTOR_VARIANT_UNKNOWN: return "unknown";
	}
	return "unknown";
}

std::string VariantStatusName(AegisubFontCollectorVariantStatus status) {
	switch (status) {
		case AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_CANONICAL: return "canonical";
		case AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_NONCANONICAL: return "noncanonical";
		case AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_SYNTHETIC: return "synthetic";
		case AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_UNKNOWN: return "unknown";
	}
	return "unknown";
}

std::string FormatCodepoint(uint32_t value) {
	std::ostringstream out;
	out << "U+" << std::uppercase << std::hex << value;
	return out.str();
}

struct JsonEvent {
	AegisubFontCollectorEventType type = AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE;
	std::string text;
	std::string face;
	std::string message;
	std::string style;
	std::string path;
	std::vector<std::string> styles;
	std::vector<int> lines;
	int count = 0;
	int requested_weight = 0;
	int requested_italic = 0;
};

struct JsonMatchCandidate {
	std::string facename;
	std::string facename_full;
	std::string matched_name;
	std::string match_source;
	std::string name_match;
	std::string path;
	int provider_order = -1;
	int face_index = -1;
	int score = 0;
	int weight = 0;
	bool bold = false;
	bool italic = false;
	std::vector<uint32_t> considered_codepoints;
	std::vector<uint32_t> supported_codepoints;
	std::vector<uint32_t> selected_codepoints;
};

struct JsonMatchedFont {
	AegisubFontCollectorMatchStatus match_status = AEGISUB_FONTCOLLECTOR_MATCH_FOUND;
	std::string facename;
	std::string facename_full;
	std::string display_name;
	std::vector<std::string> names;
	int face_index = -1;
	int weight = 0;
	bool bold = false;
	bool italic = false;
	std::string path_source;
	std::vector<std::string> paths;
	bool fake_bold = false;
	bool fake_italic = false;
	std::string missing_text;
	std::vector<uint32_t> missing_codepoints;
	std::vector<std::string> missing_codepoint_names;
	std::vector<int> missing_lines;
	int requested_weight = 0;
	bool has_backend_requested_weight = false;
	int backend_requested_weight = 0;
	std::string realized_role;
	std::string realized_status;
	bool implicit_variant_fallback = false;
	bool noncanonical_variant = false;
};

struct JsonUsage {
	std::string ass_facename;
	int ass_bold = 0;
	bool ass_italic = false;
	int ass_effective_weight = 400;
	int ass_charset = 1;
	double ass_height = 0.0;
	std::string ass_raw_bold_tag;
	std::string ass_raw_italic_tag;
	std::string ass_raw_charset_tag;
	std::string ass_raw_height_tag;
	bool ass_has_explicit_family = false;
	bool ass_has_explicit_bold = false;
	bool ass_has_explicit_italic = false;
	bool ass_has_explicit_charset = false;
	bool ass_has_explicit_height = false;
	std::string baseline_facename;
	int baseline_weight = 400;
	bool baseline_italic = false;
	int baseline_charset = 1;
	double baseline_height = 0.0;
	std::vector<uint32_t> codepoints;
	std::vector<std::string> codepoint_names;
	std::vector<std::string> styles;
	std::vector<int> lines;
	std::vector<int> override_lines;
	JsonMatchedFont matched;
	std::vector<JsonMatchCandidate> match_candidates;
	bool match_ambiguous = false;
};

struct JsonContext {
	std::string provider;
	std::string matcher;
	std::vector<JsonEvent> events;
	std::vector<JsonUsage> usages;
};

struct JsonFileReport {
	std::string input;
	int result = AEGISUB_FONTCOLLECTOR_OK;
	int exit_result = AEGISUB_FONTCOLLECTOR_OK;
	std::string error;
	AegisubFontCollectorSummary summary = {};
	JsonContext context;
};

struct NormalizationChange {
	AegisubFontNameSourceKind source_kind = AEGISUB_FONT_NAME_SOURCE_STYLE;
	std::string style;
	int line = 0;
	size_t override_index = 0;
	bool comment = false;
	std::string current_name;
	std::string recommended_name;
	AegisubFontFamilyMatchKind match_kind = AEGISUB_FONT_FAMILY_MATCH_NONE;
	std::string reason_code;
	bool safe_to_apply = false;
};

struct NormalizationFileReport {
	std::string input;
	int result = AEGISUB_FONTCOLLECTOR_OK;
	std::string error;
	AegisubFontNameNormalizationSummary summary = {};
	std::vector<NormalizationChange> changes;
};

std::string NormalizationTargetName(AegisubFontNameNormalizationTarget target) {
	return target == AEGISUB_FONT_NAME_TARGET_ENGLISH_WIN32 ? "english" : "localized";
}

std::string NormalizationSourceKindName(AegisubFontNameSourceKind kind) {
	return kind == AEGISUB_FONT_NAME_SOURCE_OVERRIDE ? "override" : "style";
}

std::string FontFamilyMatchKindName(AegisubFontFamilyMatchKind kind) {
	switch (kind) {
		case AEGISUB_FONT_FAMILY_MATCH_NONE: return "none";
		case AEGISUB_FONT_FAMILY_MATCH_EXACT: return "exact";
		case AEGISUB_FONT_FAMILY_MATCH_CASE_INSENSITIVE_EXACT: return "case_insensitive_exact";
		case AEGISUB_FONT_FAMILY_MATCH_AMBIGUOUS: return "ambiguous";
	}
	return "none";
}

void CollectNormalizationChange(
	AegisubFontNameNormalizationChange const *change,
	void *user_data)
{
	if (!change || !user_data)
		return;
	auto& changes = *static_cast<std::vector<NormalizationChange> *>(user_data);
	auto& item = changes.emplace_back();
	item.source_kind = change->source_kind;
	item.style = Safe(change->style);
	item.line = change->line;
	item.override_index = change->override_index;
	item.comment = change->comment != 0;
	item.current_name = Safe(change->current_name);
	item.recommended_name = Safe(change->recommended_name);
	item.match_kind = change->match_kind;
	item.reason_code = Safe(change->reason_code);
	item.safe_to_apply = change->safe_to_apply != 0;
}

std::string PlatformProviderName() {
#if defined(_WIN32)
	return "gdi-dwrite";
#elif defined(__APPLE__)
	return "coretext";
#else
	return "fontconfig";
#endif
}

std::string ProviderName(AegisubFontCollectorMatcher matcher) {
	if (matcher == AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS) {
#if defined(_WIN32)
		return "directwrite";
#else
		return "fontconfig";
#endif
	}
	return PlatformProviderName();
}

std::vector<std::string> ExpandFontInputs(
	std::vector<std::string> const& arguments,
	bool recursive) {
	std::vector<std::string> inputs;
	std::set<std::string> seen;
	for (auto const& argument : arguments) {
		auto path = Utf8ToPath(argument);
		std::error_code ec;
		if (std::filesystem::is_regular_file(path, ec)) {
			if (IsFontFile(path))
				AddInputFile(path, inputs, seen);
			continue;
		}
		if (!std::filesystem::is_directory(path, ec))
			continue;

		if (recursive) {
			std::filesystem::recursive_directory_iterator it(
				path, std::filesystem::directory_options::skip_permission_denied, ec);
			std::filesystem::recursive_directory_iterator end;
			for (; !ec && it != end; it.increment(ec)) {
				std::error_code file_ec;
				if (it->is_regular_file(file_ec) && !file_ec && IsFontFile(it->path()))
					AddInputFile(it->path(), inputs, seen);
			}
		}
		else {
			std::filesystem::directory_iterator it(
				path, std::filesystem::directory_options::skip_permission_denied, ec);
			std::filesystem::directory_iterator end;
			for (; !ec && it != end; it.increment(ec)) {
				std::error_code file_ec;
				if (it->is_regular_file(file_ec) && !file_ec && IsFontFile(it->path()))
					AddInputFile(it->path(), inputs, seen);
			}
		}
	}
	return inputs;
}

std::string MatcherName(AegisubFontCollectorMatcher matcher) {
	switch (matcher) {
		case AEGISUB_FONTCOLLECTOR_MATCHER_PLATFORM: return "platform";
		case AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS: return "libass";
	}
	return "unknown";
}

std::string JoinStyles(AegisubFontCollectorEvent const& event) {
	std::ostringstream out;
	for (size_t i = 0; i < event.style_count; ++i) {
		if (i)
			out << ", ";
		out << event.styles[i];
	}
	return out.str();
}

std::string JoinLines(AegisubFontCollectorEvent const& event) {
	std::ostringstream out;
	for (size_t i = 0; i < event.line_count; ++i) {
		if (i)
			out << ", ";
		out << event.lines[i];
	}
	return out.str();
}

std::string FormatEvent(AegisubFontCollectorEvent const& event) {
	switch (event.type) {
	case AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO:
			return "Font provider: " + Safe(event.message);
		case AEGISUB_FONTCOLLECTOR_EVENT_UPDATING_FONT_CACHE:
			return "Updating font cache";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR:
			return "Font cache error: " + Safe(event.message);
		case AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE:
			return "Parsing file";
		case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING:
			return "Style missing: " + Safe(event.style) +
			       (event.line_count == 1 ? " on line " + JoinLines(event) :
			        event.line_count > 1 ? " on lines " + JoinLines(event) : std::string());
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCHING_FOR_FONT_FILES:
			return "Searching for font files";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING:
			return "Missing font: " + Safe(event.face);
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_FOUND: {
			auto src = Safe(event.message);
			if (src == "memory")
				return "Found font: " + Safe(event.face) + " (memory; collectable as dumped data)";
			return "Found font: " + Safe(event.face) + " -> " + Safe(event.path) +
			       (src.empty() ? "" : " [" + src + "]");
		}
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_BOLD:
			return "Fake bold required: " + Safe(event.face) + (event.requested_weight ? " (requested " + std::to_string(event.requested_weight) + ")" : std::string());
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_ITALIC:
			return "Fake italic required: " + Safe(event.face);
		case AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS:
			return "Missing glyphs in " + Safe(event.face) + ": " +
			       (event.message && *event.message ? Safe(event.message) : std::to_string(event.count));
		case AEGISUB_FONTCOLLECTOR_EVENT_USAGE: {
			std::ostringstream out;
			if (event.style_count)
				out << "Used in styles: " << JoinStyles(event);
			if (event.line_count) {
				if (event.style_count)
					out << "; ";
				out << "Used on lines: " << JoinLines(event);
			}
			return out.str();
		}
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCH_COMPLETE:
			return "Search complete";
		case AEGISUB_FONTCOLLECTOR_EVENT_ALL_FONTS_FOUND:
			return "All fonts found";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING:
			return "Fonts missing: " + std::to_string(event.count);
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING_GLYPHS:
			return "Fonts with missing glyphs: " + std::to_string(event.count);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_FOLDER:
			return "Copying fonts to folder";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_ARCHIVE:
			return "Copying fonts to archive";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_CREATE_DIRECTORY:
			return "Failed to create directory: " + Safe(event.path) + " " + Safe(event.message);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_OPEN:
			return "Failed to open: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPIED:
			return "Copied: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_ALREADY_EXISTS:
			return "Already exists at destination: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_COPY:
			return "Failed to copy: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_ALL_COPIED:
			return "Done. All fonts copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_SOME_NOT_COPIED:
			return "Done. Some fonts could not be copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE:
			return {};
	}

	return {};
}

void CollectJsonEvent(AegisubFontCollectorEvent const *event, void *user_data) {
	if (!event || !user_data)
		return;

	auto& context = *static_cast<JsonContext *>(user_data);
	auto& item = context.events.emplace_back();
	item.type = event->type;
	item.text = FormatEvent(*event);
	item.face = Safe(event->face);
	item.message = Safe(event->message);
	item.style = Safe(event->style);
	item.path = Safe(event->path);
	item.styles.reserve(event->style_count);
	for (size_t i = 0; i < event->style_count; ++i)
		item.styles.emplace_back(event->styles[i]);
	if (event->line_count)
		item.lines.assign(event->lines, event->lines + event->line_count);
	item.count = event->count;
	item.requested_weight = event->requested_weight;
	item.requested_italic = event->requested_italic;
}

void CollectJsonUsage(AegisubFontCollectorFontUsage const *usage, void *user_data) {
	if (!usage || !user_data)
		return;

	auto& context = *static_cast<JsonContext *>(user_data);
	auto& item = context.usages.emplace_back();
	item.ass_facename = Safe(usage->ass_facename);
	item.ass_bold = usage->ass_bold;
	item.ass_italic = usage->ass_italic != 0;
	item.ass_effective_weight = usage->ass_effective_weight;
	item.ass_charset = usage->ass_charset;
	item.ass_height = usage->ass_height;
	item.ass_raw_bold_tag = Safe(usage->ass_raw_bold_tag);
	item.ass_raw_italic_tag = Safe(usage->ass_raw_italic_tag);
	item.ass_raw_charset_tag = Safe(usage->ass_raw_charset_tag);
	item.ass_raw_height_tag = Safe(usage->ass_raw_height_tag);
	item.ass_has_explicit_family = usage->ass_has_explicit_family != 0;
	item.ass_has_explicit_bold = usage->ass_has_explicit_bold != 0;
	item.ass_has_explicit_italic = usage->ass_has_explicit_italic != 0;
	item.ass_has_explicit_charset = usage->ass_has_explicit_charset != 0;
	item.ass_has_explicit_height = usage->ass_has_explicit_height != 0;
	item.baseline_facename = Safe(usage->baseline_facename);
	item.baseline_weight = usage->baseline_weight;
	item.baseline_italic = usage->baseline_italic != 0;
	item.baseline_charset = usage->baseline_charset;
	item.baseline_height = usage->baseline_height;
	if (usage->codepoint_count)
		item.codepoints.assign(usage->codepoints, usage->codepoints + usage->codepoint_count);
	item.codepoint_names.reserve(item.codepoints.size());
	for (auto codepoint : item.codepoints)
		item.codepoint_names.push_back(FormatCodepoint(codepoint));
	item.styles.reserve(usage->style_count);
	for (size_t i = 0; i < usage->style_count; ++i)
		item.styles.emplace_back(usage->styles[i]);
	if (usage->line_count)
		item.lines.assign(usage->lines, usage->lines + usage->line_count);
	if (usage->override_line_count)
		item.override_lines.assign(usage->override_lines, usage->override_lines + usage->override_line_count);
	item.matched.match_status = usage->matched.match_status;
	item.matched.facename = Safe(usage->matched.facename);
	item.matched.facename_full = Safe(usage->matched_facename_full);
	if (usage->matched_names) {
		item.matched.names.reserve(usage->matched_name_count);
		for (size_t i = 0; i < usage->matched_name_count; ++i)
			item.matched.names.emplace_back(usage->matched_names[i]);
	}
	item.matched.face_index = usage->matched.face_index;
	item.matched.weight = usage->matched.weight;
	item.matched.bold = usage->matched.bold != 0;
	item.matched.italic = usage->matched.italic != 0;
	item.matched.path_source = Safe(usage->matched.path_source);
	item.matched.paths.reserve(usage->matched.path_count);
	for (size_t i = 0; i < usage->matched.path_count; ++i)
		item.matched.paths.emplace_back(usage->matched.paths[i]);
	item.matched.fake_bold = usage->matched.fake_bold != 0;
	item.matched.fake_italic = usage->matched.fake_italic != 0;
	item.matched.missing_text = Safe(usage->matched.missing_text);
	if (usage->matched.missing_codepoint_count)
		item.matched.missing_codepoints.assign(usage->matched.missing_codepoints, usage->matched.missing_codepoints + usage->matched.missing_codepoint_count);
	item.matched.missing_codepoint_names.reserve(item.matched.missing_codepoints.size());
	for (auto codepoint : item.matched.missing_codepoints)
		item.matched.missing_codepoint_names.push_back(FormatCodepoint(codepoint));
	if (usage->matched.missing_line_count)
		item.matched.missing_lines.assign(usage->matched.missing_lines, usage->matched.missing_lines + usage->matched.missing_line_count);
	item.matched.requested_weight = usage->matched.requested_weight;
	item.matched.has_backend_requested_weight = usage->matched_has_backend_requested_weight != 0;
	item.matched.backend_requested_weight = usage->matched_backend_requested_weight;
	if (usage->matched_realized_role != AEGISUB_FONTCOLLECTOR_VARIANT_UNKNOWN)
		item.matched.realized_role = VariantRoleName(usage->matched_realized_role);
	if (usage->matched_realized_status != AEGISUB_FONTCOLLECTOR_VARIANT_STATUS_UNKNOWN)
		item.matched.realized_status = VariantStatusName(usage->matched_realized_status);
	item.matched.implicit_variant_fallback = usage->matched_implicit_variant_fallback != 0;
	item.matched.noncanonical_variant = usage->matched_noncanonical_variant != 0;
	item.match_candidates.reserve(usage->match_candidate_count);
	for (size_t i = 0; i < usage->match_candidate_count; ++i) {
		auto const& source = usage->match_candidates[i];
		auto& candidate = item.match_candidates.emplace_back();
		candidate.facename = Safe(source.facename);
		candidate.facename_full = Safe(source.facename_full);
		candidate.matched_name = Safe(source.matched_name);
		candidate.match_source = Safe(source.match_source);
		candidate.name_match = Safe(source.name_match);
		candidate.path = Safe(source.path);
		candidate.provider_order = source.provider_order;
		candidate.face_index = source.face_index;
		candidate.score = source.score;
		candidate.weight = source.weight;
		candidate.bold = source.bold != 0;
		candidate.italic = source.italic != 0;
		if (source.considered_codepoint_count)
			candidate.considered_codepoints.assign(
				source.considered_codepoints,
				source.considered_codepoints + source.considered_codepoint_count);
		if (source.supported_codepoint_count)
			candidate.supported_codepoints.assign(
				source.supported_codepoints,
				source.supported_codepoints + source.supported_codepoint_count);
		if (source.selected_codepoint_count)
			candidate.selected_codepoints.assign(
				source.selected_codepoints,
				source.selected_codepoints + source.selected_codepoint_count);
	}
	item.match_ambiguous = usage->match_ambiguous != 0;
}

std::string JoinStrings(std::vector<std::string> const& values) {
	std::ostringstream out;
	for (size_t i = 0; i < values.size(); ++i) {
		if (i)
			out << ", ";
		out << values[i];
	}
	return out.str();
}

std::string JoinInts(std::vector<int> const& values) {
	std::ostringstream out;
	for (size_t i = 0; i < values.size(); ++i) {
		if (i)
			out << ", ";
		out << values[i];
	}
	return out.str();
}

std::string JoinCodepoints(std::vector<uint32_t> const& values) {
	std::ostringstream out;
	for (size_t i = 0; i < values.size(); ++i) {
		if (i)
			out << ' ';
		out << "U+" << std::uppercase << std::hex << values[i] << std::dec;
	}
	return out.str();
}

std::string LowerAsciiCopy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

std::string CountLabel(uint64_t count, char const *singular, char const *plural) {
	return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

std::string LocationText(std::string const& input, std::vector<int> const& lines) {
	if (lines.empty())
		return input;
	if (lines.size() == 1)
		return input + ":" + std::to_string(lines.front());
	return input + ":lines " + JoinInts(lines);
}

bool HasStrictFindings(AegisubFontCollectorSummary const& summary) {
	return summary.missing_style_count || summary.missing_font_count ||
	       summary.missing_glyph_font_count || summary.collection_failure_count;
}

std::string SummaryText(AegisubFontCollectorSummary const& summary) {
	std::vector<std::string> parts;
	parts.push_back(CountLabel(static_cast<uint64_t>(summary.font_usage_count), "font request", "font requests"));
	parts.push_back(CountLabel(static_cast<uint64_t>(summary.found_font_count), "font found", "fonts found"));
	if (summary.missing_style_count)
		parts.push_back(CountLabel(static_cast<uint64_t>(summary.missing_style_count), "missing style", "missing styles"));
	if (summary.missing_font_count)
		parts.push_back(CountLabel(static_cast<uint64_t>(summary.missing_font_count), "missing font", "missing fonts"));
	if (summary.missing_glyph_font_count)
		parts.push_back(CountLabel(static_cast<uint64_t>(summary.missing_glyph_font_count), "font with missing glyphs", "fonts with missing glyphs"));
	if (summary.fake_bold_count)
		parts.push_back(CountLabel(static_cast<uint64_t>(summary.fake_bold_count), "fake bold", "fake bold"));
	if (summary.fake_italic_count)
		parts.push_back(CountLabel(static_cast<uint64_t>(summary.fake_italic_count), "fake italic", "fake italic"));
	if (summary.copied_font_count)
		parts.push_back(CountLabel(summary.copied_font_count, "font copied", "fonts copied"));
	if (summary.collection_failure_count)
		parts.push_back(CountLabel(summary.collection_failure_count, "copy failure", "copy failures"));
	return JoinStrings(parts);
}

std::vector<int> FollowingUsageLines(std::vector<JsonEvent> const& events, size_t index) {
	for (size_t i = index + 1; i < events.size(); ++i) {
		if (events[i].type == AEGISUB_FONTCOLLECTOR_EVENT_USAGE)
			return events[i].lines;
		switch (events[i].type) {
			case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING:
			case AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING:
			case AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS:
			case AEGISUB_FONTCOLLECTOR_EVENT_SEARCH_COMPLETE:
			case AEGISUB_FONTCOLLECTOR_EVENT_ALL_FONTS_FOUND:
			case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING:
			case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING_GLYPHS:
			case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_ALL_COPIED:
			case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_SOME_NOT_COPIED:
				return {};
			default:
				break;
		}
	}
	return {};
}

void PrintStoredUsage(JsonUsage const& usage) {
	std::cout << "Font usage: " << usage.ass_facename
		<< " weight=" << usage.ass_bold
		<< " italic=" << usage.ass_italic << "\n";
	if (!usage.styles.empty())
		std::cout << "  styles: " << JoinStrings(usage.styles) << "\n";
	if (!usage.lines.empty())
		std::cout << "  lines: " << JoinInts(usage.lines) << "\n";
	if (!usage.override_lines.empty())
		std::cout << "  override lines: " << JoinInts(usage.override_lines) << "\n";
	std::cout << "  codepoints: " << JoinCodepoints(usage.codepoints) << "\n";
	std::cout << "  matched: " << usage.matched.facename
		<< " status=" << MatchStatusName(usage.matched.match_status)
		<< (!usage.matched.facename_full.empty() && usage.matched.facename_full != usage.matched.facename ? " full=" + usage.matched.facename_full : std::string())
		<< " face_index=" << usage.matched.face_index
		<< " weight=" << usage.matched.weight
		<< " italic=" << usage.matched.italic
		<< " fake_bold=" << usage.matched.fake_bold
		<< " fake_italic=" << usage.matched.fake_italic << "\n";
	if (usage.matched.implicit_variant_fallback)
		std::cout << "  portability risk: implicit non-regular variant fallback\n";
	if (usage.matched.noncanonical_variant)
		std::cout << "  portability risk: noncanonical realized variant\n";
	if (!usage.matched.realized_role.empty() || !usage.matched.realized_status.empty())
		std::cout << "  realized variant: " << usage.matched.realized_role
		          << " status=" << usage.matched.realized_status << "\n";
	if (!usage.matched.display_name.empty())
		std::cout << "  display: " << usage.matched.display_name << "\n";
	if (!usage.matched.names.empty())
		std::cout << "  names: " << JoinStrings(usage.matched.names) << "\n";
	if (!usage.matched.paths.empty())
		std::cout << "  paths: " << JoinStrings(usage.matched.paths) << "\n";
	if (!usage.matched.missing_text.empty())
		std::cout << "  missing codepoints: " << usage.matched.missing_text << "\n";
	if (!usage.matched.missing_lines.empty())
		std::cout << "  missing lines: " << JoinInts(usage.matched.missing_lines) << "\n";
	if (!usage.match_candidates.empty()) {
		std::cout << "  match candidates (lower score is better)"
		          << (usage.match_ambiguous ? " [ambiguous]" : "") << ":\n";
		for (auto const& candidate : usage.match_candidates) {
			std::cout << "    " << candidate.facename
			          << " score=" << candidate.score
			          << " source=" << candidate.match_source
			          << " name_match=" << candidate.name_match
			          << " provider_order=" << candidate.provider_order
			          << " weight=" << candidate.weight
			          << " italic=" << candidate.italic
			          << " supported=" << candidate.supported_codepoints.size()
			          << "/" << candidate.considered_codepoints.size()
			          << " selected=" << candidate.selected_codepoints.size() << "\n";
		}
	}
}

std::string ListStatus(JsonUsage const& usage) {
	return usage.matched.match_status == AEGISUB_FONTCOLLECTOR_MATCH_MISSING ? "missing" : "installed";
}

bool SameFontName(std::string const& a, std::string const& b) {
	return LowerAsciiCopy(a) == LowerAsciiCopy(b);
}

std::string AssFontInfoText(JsonUsage const& usage) {
	return usage.ass_facename + "," + std::to_string(usage.ass_bold) + "," + (usage.ass_italic ? "1" : "0");
}

struct ListFontEntry {
	std::string name;
	std::string sort_key;
	bool missing = true;
	std::vector<JsonUsage const*> usages;
};

void AddListUsage(std::vector<ListFontEntry>& entries, std::unordered_map<std::string, size_t>& index, JsonUsage const& usage) {
	auto key = LowerAsciiCopy(usage.ass_facename);
	auto [it, inserted] = index.emplace(key, entries.size());
	if (inserted) {
		ListFontEntry entry;
		entry.name = usage.ass_facename;
		entry.sort_key = key;
		entries.push_back(std::move(entry));
	}

	auto& entry = entries[it->second];
	entry.missing = entry.missing && usage.matched.match_status == AEGISUB_FONTCOLLECTOR_MATCH_MISSING;
	entry.usages.push_back(&usage);
}

std::vector<ListFontEntry> BuildListEntries(std::vector<JsonFileReport> const& reports) {
	std::vector<ListFontEntry> entries;
	std::unordered_map<std::string, size_t> index;
	for (auto const& report : reports) {
		if (report.result != AEGISUB_FONTCOLLECTOR_OK)
			continue;
		for (auto const& usage : report.context.usages)
			AddListUsage(entries, index, usage);
	}
	std::sort(entries.begin(), entries.end(), [](ListFontEntry const& a, ListFontEntry const& b) {
		return a.sort_key < b.sort_key;
	});
	return entries;
}

JsonUsage const *FirstInstalledUsage(ListFontEntry const& entry) {
	auto it = std::find_if(entry.usages.begin(), entry.usages.end(), [](JsonUsage const *usage) {
		return usage->matched.match_status != AEGISUB_FONTCOLLECTOR_MATCH_MISSING;
	});
	return it == entry.usages.end() ? nullptr : *it;
}

bool HasMissingGlyphs(ListFontEntry const& entry) {
	return std::any_of(entry.usages.begin(), entry.usages.end(), [](JsonUsage const *usage) {
		return !usage->matched.missing_codepoints.empty();
	});
}

std::string ListDisplayName(ListFontEntry const& entry) {
	if (auto const *usage = FirstInstalledUsage(entry)) {
		if (!usage->matched.display_name.empty())
			return usage->matched.display_name;
		if (!usage->matched.facename.empty())
			return usage->matched.facename;
	}
	return entry.name;
}

bool StdoutSupportsColor() {
#ifdef _WIN32
	auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
	if (handle == INVALID_HANDLE_VALUE || !handle)
		return false;
	DWORD mode = 0;
	if (!GetConsoleMode(handle, &mode))
		return false;
	if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
		return true;
	return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
	return false;
#endif
}

char const *ListColor(ListFontEntry const& entry) {
	if (entry.missing)
		return "\x1b[31m";
	if (HasMissingGlyphs(entry))
		return "\x1b[34m";
	return "";
}

void PrintListDiagnostics(std::vector<JsonFileReport> const& reports) {
	for (auto const& report : reports) {
		if (report.result != AEGISUB_FONTCOLLECTOR_OK) {
			std::cerr << "fontcollector failed for " << report.input;
			if (!report.error.empty())
				std::cerr << ": " << report.error;
			std::cerr << "\n";
			continue;
		}

		for (auto const& usage : report.context.usages) {
			if (usage.matched.missing_codepoints.empty())
				continue;

			auto lines = usage.matched.missing_lines.empty() ? std::string() : " Dialogue #" + JoinInts(usage.matched.missing_lines);
			std::cerr << "AssFontInfo '" << AssFontInfoText(usage) << "'" << lines
			          << " is missing characters: " << usage.matched.missing_text << "\n";
		}
	}
}

void PrintListEntry(ListFontEntry const& entry, bool details) {
	auto const color = StdoutSupportsColor() ? ListColor(entry) : "";
	if (*color)
		std::cout << color;
	if (entry.missing)
		std::cout << entry.name;
	else
		std::cout << ListDisplayName(entry) << " <" << entry.name << ">";
	if (*color)
		std::cout << "\x1b[0m";
	std::cout << "\n";
	if (!details)
		return;

	for (auto const *usage : entry.usages) {
		std::cout << "  " << usage->ass_facename
			<< ", weight=" << usage->ass_bold
			<< ", italic=" << (usage->ass_italic ? "true" : "false")
			<< ", " << ListStatus(*usage) << "\n";
		if (!usage->matched.facename.empty())
			std::cout << "    matched: " << usage->matched.facename << "\n";
		if (!usage->matched.facename_full.empty() && usage->matched.facename_full != usage->matched.facename)
			std::cout << "    full: " << usage->matched.facename_full << "\n";
		if (!usage->matched.display_name.empty() && !SameFontName(usage->matched.display_name, usage->ass_facename))
			std::cout << "    display: " << usage->matched.display_name << "\n";
		if (!usage->matched.names.empty()) {
			std::cout << "    names:\n";
			for (auto const& name : usage->matched.names)
				std::cout << "      " << name << "\n";
		}
		if (!usage->matched.path_source.empty())
			std::cout << "    source: " << usage->matched.path_source << "\n";
		if (usage->matched.implicit_variant_fallback)
			std::cout << "    portability risk: implicit non-regular variant fallback\n";
		if (usage->matched.noncanonical_variant)
			std::cout << "    portability risk: noncanonical realized variant\n";
		if (!usage->matched.realized_role.empty() || !usage->matched.realized_status.empty())
			std::cout << "    realized variant: " << usage->matched.realized_role
			          << " status=" << usage->matched.realized_status << "\n";
		if (!usage->styles.empty())
			std::cout << "    styles: " << JoinStrings(usage->styles) << "\n";
		if (!usage->lines.empty())
			std::cout << "    lines: " << JoinInts(usage->lines) << "\n";
		if (!usage->override_lines.empty())
			std::cout << "    override lines: " << JoinInts(usage->override_lines) << "\n";
		for (auto const& path : usage->matched.paths)
			std::cout << "    " << path << "\n";
	}
}

void PrintListReports(std::vector<JsonFileReport> const& reports, bool details, bool quiet) {
	if (!quiet) {
		for (auto const& entry : BuildListEntries(reports))
			PrintListEntry(entry, details);
	}

	PrintListDiagnostics(reports);
}

std::string BestDisplayName(JsonUsage const& usage) {
	if (!usage.matched.facename.empty())
		return usage.matched.facename;
	return usage.ass_facename;
}

void PopulateMatchedFontNames(std::vector<JsonFileReport>& reports) {
	for (auto& report : reports) {
		for (auto& usage : report.context.usages) {
			auto& matched = usage.matched;
			auto names = std::move(matched.names);
			AddUnique(names, matched.facename);
			AddUnique(names, matched.facename_full);
			matched.display_name = BestDisplayName(usage);
			matched.names = std::move(names);
		}
	}
}

bool PrintHumanDiagnostic(JsonFileReport const& report, size_t index) {
	auto const& event = report.context.events[index];
	switch (event.type) {
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR:
			std::cout << "ERROR: Font cache error";
			if (!event.message.empty())
				std::cout << ": " << event.message;
			std::cout << "\n";
			return true;
		case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING:
			std::cout << "ERROR: Missing style \"" << event.style << "\" at " << LocationText(report.input, event.lines) << "\n";
			return true;
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING: {
			auto lines = FollowingUsageLines(report.context.events, index);
			std::cout << "ERROR: Missing font \"" << event.face << "\"";
			if (event.requested_weight)
				std::cout << " weight=" << event.requested_weight;
			if (event.requested_italic)
				std::cout << " italic=1";
			std::cout << " at " << LocationText(report.input, lines) << "\n";
			return true;
		}
		case AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS: {
			auto lines = FollowingUsageLines(report.context.events, index);
			std::cout << "ERROR: Missing glyphs for \"" << event.face << "\" at " << LocationText(report.input, lines);
			if (!event.message.empty())
				std::cout << ": " << event.message;
			else if (event.count)
				std::cout << ": " << event.count;
			std::cout << "\n";
			return true;
		}
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_BOLD:
			std::cout << "WARN: Fake bold required for \"" << event.face << "\"";
			if (event.requested_weight)
				std::cout << " weight=" << event.requested_weight;
			std::cout << " at " << LocationText(report.input, FollowingUsageLines(report.context.events, index)) << "\n";
			return true;
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_ITALIC:
			std::cout << "WARN: Fake italic required for \"" << event.face << "\" at "
			          << LocationText(report.input, FollowingUsageLines(report.context.events, index)) << "\n";
			return true;
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_CREATE_DIRECTORY:
			std::cout << "ERROR: Failed to create directory";
			if (!event.path.empty())
				std::cout << " \"" << event.path << "\"";
			if (!event.message.empty())
				std::cout << ": " << event.message;
			std::cout << "\n";
			return true;
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_OPEN:
			std::cout << "ERROR: Failed to open font";
			if (!event.path.empty())
				std::cout << " \"" << event.path << "\"";
			std::cout << " for " << report.input << "\n";
			return true;
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_COPY:
			std::cout << "ERROR: Failed to copy font";
			if (!event.path.empty())
				std::cout << " \"" << event.path << "\"";
			std::cout << " for " << report.input << "\n";
			return true;
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_SOME_NOT_COPIED:
			std::cout << "ERROR: Some fonts could not be copied for " << report.input << "\n";
			return true;
		default:
			return false;
	}
}

void PrintHumanSummary(JsonFileReport const& report) {
	auto const *label = report.result != AEGISUB_FONTCOLLECTOR_OK
		? "ERROR"
		: HasStrictFindings(report.summary) ? "ISSUES" : "OK";
	std::cout << label << ": " << report.input << " - " << SummaryText(report.summary) << "\n";
}

void PrintStoredReport(JsonFileReport const& report, bool show_header, bool details) {
	if (show_header)
		std::cout << "== " << report.input << " ==\n";

	if (report.result != AEGISUB_FONTCOLLECTOR_OK) {
		std::cerr << "fontcollector failed for " << report.input;
		if (!report.error.empty())
			std::cerr << ": " << report.error;
		std::cerr << "\n";
		return;
	}

	if (details) {
		for (auto const& event : report.context.events) {
			if (event.text.empty())
				std::cout << "\n";
			else
				std::cout << event.text << "\n";
		}

		for (auto const& usage : report.context.usages)
			PrintStoredUsage(usage);

		PrintHumanSummary(report);
		return;
	}

	for (size_t i = 0; i < report.context.events.size(); ++i)
		PrintHumanDiagnostic(report, i);
	for (auto const& usage : report.context.usages) {
		if (usage.match_ambiguous)
			std::cout << "WARN: ambiguous libass font match for " << usage.ass_facename
			          << "; use --details or --json to inspect equal-score candidates\n";
		if (usage.matched.implicit_variant_fallback)
			std::cout << "WARN: implicit non-regular variant fallback for " << usage.ass_facename
			          << "; make the ASS variant explicit for portable rendering\n";
		if (usage.matched.noncanonical_variant)
			std::cout << "WARN: noncanonical realized font variant for " << usage.ass_facename
			          << "; automatic ASS variant normalization is unsafe\n";
	}

	PrintHumanSummary(report);
}

struct JsonDiagnosticFont {
	std::string facename;
	int bold = 0;
	bool italic = false;
	int requested_weight = 0;
};

struct JsonDiagnostic {
	std::string type;
	std::string severity = "error";
	std::string file;
	std::vector<int> lines;
	bool has_style = false;
	std::string style;
	bool has_font = false;
	JsonDiagnosticFont font;
	std::string message;
	std::string text;
	std::vector<uint32_t> codepoints;
	std::vector<std::string> codepoint_names;
	bool has_path = false;
	std::string path;
	bool has_result = false;
	int result = AEGISUB_FONTCOLLECTOR_OK;
	size_t match_candidate_count = 0;
	bool match_ambiguous = false;
};

JsonDiagnostic MakeDiagnostic(std::string type, std::string severity, std::string const& file, std::string message) {
	JsonDiagnostic diagnostic;
	diagnostic.type = std::move(type);
	diagnostic.severity = std::move(severity);
	diagnostic.file = file;
	diagnostic.message = std::move(message);
	return diagnostic;
}

JsonDiagnosticFont UsageFont(JsonUsage const& usage) {
	JsonDiagnosticFont font;
	font.facename = usage.ass_facename;
	font.bold = usage.ass_bold;
	font.italic = usage.ass_italic;
	font.requested_weight = usage.matched.requested_weight;
	return font;
}

json::Integer JsonInt(uint64_t value) {
	return static_cast<json::Integer>(value);
}

json::UnknownElement JsonStringOrNull(bool present, std::string const& value) {
	return present ? json::UnknownElement(value) : json::UnknownElement(json::Null());
}

json::UnknownElement JsonIntOrNull(bool present, int value) {
	return present ? json::UnknownElement(value) : json::UnknownElement(json::Null());
}

json::Array ToJson(std::vector<std::string> const& values) {
	json::Array array;
	array.reserve(values.size());
	for (auto const& value : values)
		array.emplace_back(value);
	return array;
}

json::Array ToJson(std::vector<int> const& values) {
	json::Array array;
	array.reserve(values.size());
	for (auto value : values)
		array.emplace_back(value);
	return array;
}

json::Array CodepointValuesJson(std::vector<uint32_t> const& values) {
	json::Array array;
	array.reserve(values.size());
	for (auto value : values)
		array.emplace_back(JsonInt(value));
	return array;
}

json::Object CodepointsJson(std::vector<uint32_t> const& values, std::vector<std::string> const& names) {
	json::Object object;
	object["names"] = ToJson(names);
	object["values"] = CodepointValuesJson(values);
	return object;
}

json::Object SummaryJson(AegisubFontCollectorSummary const& summary) {
	json::Object object;
	object["collection_failure_count"] = JsonInt(summary.collection_failure_count);
	object["copied_font_count"] = JsonInt(summary.copied_font_count);
	object["fake_bold_count"] = JsonInt(summary.fake_bold_count);
	object["fake_italic_count"] = JsonInt(summary.fake_italic_count);
	object["font_usage_count"] = JsonInt(summary.font_usage_count);
	object["found_font_count"] = JsonInt(summary.found_font_count);
	object["missing_font_count"] = JsonInt(summary.missing_font_count);
	object["missing_glyph_font_count"] = JsonInt(summary.missing_glyph_font_count);
	object["missing_style_count"] = JsonInt(summary.missing_style_count);
	return object;
}

std::vector<std::string> CodepointNames(std::vector<uint32_t> const& values) {
	std::vector<std::string> names;
	names.reserve(values.size());
	for (auto value : values)
		names.push_back(FormatCodepoint(value));
	return names;
}

json::Object ToJson(JsonMatchCandidate const& candidate) {
	json::Object object;
	object["bold"] = candidate.bold;
	object["considered_codepoints"] = CodepointsJson(
		candidate.considered_codepoints, CodepointNames(candidate.considered_codepoints));
	object["face_index"] = candidate.face_index;
	object["facename"] = candidate.facename;
	object["facename_full"] = candidate.facename_full;
	object["italic"] = candidate.italic;
	object["match_source"] = candidate.match_source;
	object["matched_name"] = candidate.matched_name;
	object["name_match"] = candidate.name_match;
	object["path"] = candidate.path;
	object["provider_order"] = candidate.provider_order;
	object["score"] = candidate.score;
	object["selected_codepoints"] = CodepointsJson(
		candidate.selected_codepoints, CodepointNames(candidate.selected_codepoints));
	object["supported_codepoints"] = CodepointsJson(
		candidate.supported_codepoints, CodepointNames(candidate.supported_codepoints));
	object["weight"] = candidate.weight;
	return object;
}

json::Array MatchCandidatesJson(std::vector<JsonMatchCandidate> const& candidates) {
	json::Array array;
	array.reserve(candidates.size());
	for (auto const& candidate : candidates)
		array.emplace_back(ToJson(candidate));
	return array;
}

json::Object ToJson(JsonDiagnosticFont const& font) {
	json::Object object;
	object["bold"] = font.bold;
	object["facename"] = font.facename;
	object["italic"] = font.italic;
	object["requested_weight"] = font.requested_weight;
	return object;
}

json::Object ToJson(JsonDiagnostic const& diagnostic) {
	json::Object object;
	object["codepoints"] = CodepointsJson(diagnostic.codepoints, diagnostic.codepoint_names);
	object["file"] = diagnostic.file;
	object["font"] = diagnostic.has_font ? json::UnknownElement(ToJson(diagnostic.font)) : json::UnknownElement(json::Null());
	object["lines"] = ToJson(diagnostic.lines);
	object["message"] = diagnostic.message;
	object["path"] = JsonStringOrNull(diagnostic.has_path, diagnostic.path);
	object["result"] = JsonIntOrNull(diagnostic.has_result, diagnostic.result);
	object["severity"] = diagnostic.severity;
	object["style"] = JsonStringOrNull(diagnostic.has_style, diagnostic.style);
	object["text"] = diagnostic.text;
	object["type"] = diagnostic.type;
	if (diagnostic.match_candidate_count) {
		json::Object match_analysis;
		match_analysis["ambiguous"] = diagnostic.match_ambiguous;
		match_analysis["candidate_count"] = JsonInt(diagnostic.match_candidate_count);
		match_analysis["score_order"] = "lower_is_better";
		object["match_analysis"] = std::move(match_analysis);
	}
	return object;
}

json::Array DiagnosticsJson(std::vector<JsonDiagnostic> const& diagnostics) {
	json::Array array;
	array.reserve(diagnostics.size());
	for (auto const& diagnostic : diagnostics)
		array.emplace_back(ToJson(diagnostic));
	return array;
}

std::vector<JsonDiagnostic> BuildDiagnostics(JsonFileReport const& report) {
	std::vector<JsonDiagnostic> diagnostics;

	if (report.result != AEGISUB_FONTCOLLECTOR_OK) {
		auto diagnostic = MakeDiagnostic("file_error", "error", report.input, report.error);
		diagnostic.has_result = true;
		diagnostic.result = report.result;
		diagnostics.push_back(std::move(diagnostic));
	}

	for (auto const& event : report.context.events) {
		switch (event.type) {
			case AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR: {
				auto diagnostic = MakeDiagnostic("font_cache_error", "error", report.input, event.message);
				diagnostics.push_back(std::move(diagnostic));
				break;
			}
			case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING: {
				auto diagnostic = MakeDiagnostic("missing_style", "error", report.input, "style is referenced but not defined");
				diagnostic.lines = event.lines;
				diagnostic.has_style = true;
				diagnostic.style = event.style;
				diagnostics.push_back(std::move(diagnostic));
				break;
			}
			case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_CREATE_DIRECTORY:
			case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_OPEN:
			case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_COPY: {
				auto diagnostic = MakeDiagnostic(EventTypeName(event.type), "error", report.input, event.text);
				diagnostic.has_path = !event.path.empty();
				diagnostic.path = event.path;
				diagnostics.push_back(std::move(diagnostic));
				break;
			}
			default:
				break;
		}
	}

	for (auto const& usage : report.context.usages) {
		if (usage.match_candidates.size() > 1) {
			auto diagnostic = MakeDiagnostic(
				usage.match_ambiguous ? "ambiguous_font_match" : "font_match_candidates",
				usage.match_ambiguous ? "warning" : "info",
				report.input,
				usage.match_ambiguous
					? "equal-score candidates can serve the same codepoint; provider order selected the face"
					: "font was selected from multiple candidates; the libass score is a ranking, not proof of identity");
			diagnostic.lines = usage.lines;
			diagnostic.has_font = true;
			diagnostic.font = UsageFont(usage);
			diagnostic.match_candidate_count = usage.match_candidates.size();
			diagnostic.match_ambiguous = usage.match_ambiguous;
			diagnostics.push_back(std::move(diagnostic));
		}

		if (usage.matched.match_status == AEGISUB_FONTCOLLECTOR_MATCH_MISSING) {
			auto diagnostic = MakeDiagnostic("missing_font", "error", report.input, "font request could not be resolved");
			diagnostic.lines = usage.lines;
			diagnostic.has_font = true;
			diagnostic.font = UsageFont(usage);
			diagnostics.push_back(std::move(diagnostic));
		}

		if (!usage.matched.missing_codepoints.empty()) {
			auto diagnostic = MakeDiagnostic("missing_glyphs", "error", report.input, "matched font is missing required glyphs");
			diagnostic.lines = usage.matched.missing_lines.empty() ? usage.lines : usage.matched.missing_lines;
			diagnostic.has_font = true;
			diagnostic.font = UsageFont(usage);
			diagnostic.text = usage.matched.missing_text;
			diagnostic.codepoints = usage.matched.missing_codepoints;
			diagnostic.codepoint_names = usage.matched.missing_codepoint_names;
			diagnostics.push_back(std::move(diagnostic));
		}

		if (usage.matched.fake_bold) {
			auto diagnostic = MakeDiagnostic("fake_bold", "warning", report.input, "matched font needs synthetic bold");
			diagnostic.lines = usage.lines;
			diagnostic.has_font = true;
			diagnostic.font = UsageFont(usage);
			diagnostics.push_back(std::move(diagnostic));
		}

		if (usage.matched.fake_italic) {
			auto diagnostic = MakeDiagnostic("fake_italic", "warning", report.input, "matched font needs synthetic italic");
			diagnostic.lines = usage.lines;
			diagnostic.has_font = true;
			diagnostic.font = UsageFont(usage);
			diagnostics.push_back(std::move(diagnostic));
		}

		if (usage.matched.implicit_variant_fallback) {
			auto diagnostic = MakeDiagnostic(
				"implicit_variant_fallback",
				"warning",
				report.input,
				"font family resolved to a non-regular variant without an explicit ASS variant");
			diagnostic.lines = usage.lines;
			diagnostic.has_font = true;
			diagnostic.font = UsageFont(usage);
			diagnostics.push_back(std::move(diagnostic));
		}

		if (usage.matched.noncanonical_variant) {
			auto diagnostic = MakeDiagnostic(
				"noncanonical_font_variant",
				"warning",
				report.input,
				"realized font weight or style cannot be safely represented as Regular/Bold/Italic");
			diagnostic.lines = usage.lines;
			diagnostic.has_font = true;
			diagnostic.font = UsageFont(usage);
			diagnostics.push_back(std::move(diagnostic));
		}
	}

	return diagnostics;
}

AegisubFontCollectorSummary AggregateSummary(std::vector<JsonFileReport> const& reports) {
	AegisubFontCollectorSummary summary = {};
	for (auto const& report : reports) {
		summary.font_usage_count += report.summary.font_usage_count;
		summary.found_font_count += report.summary.found_font_count;
		summary.missing_style_count += report.summary.missing_style_count;
		summary.missing_font_count += report.summary.missing_font_count;
		summary.missing_glyph_font_count += report.summary.missing_glyph_font_count;
		summary.fake_bold_count += report.summary.fake_bold_count;
		summary.fake_italic_count += report.summary.fake_italic_count;
		summary.copied_font_count += report.summary.copied_font_count;
		summary.collection_failure_count += report.summary.collection_failure_count;
	}
	return summary;
}

json::Object ToJson(JsonEvent const& event) {
	json::Object object;
	object["count"] = event.count;
	object["face"] = event.face;
	object["lines"] = ToJson(event.lines);
	object["message"] = event.message;
	object["path"] = event.path;
	object["requested_italic"] = event.requested_italic;
	object["requested_weight"] = event.requested_weight;
	object["style"] = event.style;
	object["styles"] = ToJson(event.styles);
	object["text"] = event.text;
	object["type"] = static_cast<int>(event.type);
	object["type_name"] = EventTypeName(event.type);
	return object;
}

json::Object ToJson(JsonUsage const& usage) {
	json::Object ass_font;
	ass_font["bold"] = usage.ass_bold;
	ass_font["charset"] = usage.ass_charset;
	ass_font["effective_weight"] = usage.ass_effective_weight;
	ass_font["facename"] = usage.ass_facename;
	ass_font["height"] = usage.ass_height;
	ass_font["italic"] = usage.ass_italic;
	if (usage.ass_has_explicit_family)
		ass_font["has_explicit_family"] = true;
	if (usage.ass_has_explicit_bold)
		ass_font["raw_bold_tag"] = usage.ass_raw_bold_tag;
	if (usage.ass_has_explicit_italic)
		ass_font["raw_italic_tag"] = usage.ass_raw_italic_tag;
	if (usage.ass_has_explicit_charset)
		ass_font["raw_charset_tag"] = usage.ass_raw_charset_tag;
	if (usage.ass_has_explicit_height)
		ass_font["raw_height_tag"] = usage.ass_raw_height_tag;
	json::Object baseline;
	baseline["charset"] = usage.baseline_charset;
	baseline["facename"] = usage.baseline_facename;
	baseline["height"] = usage.baseline_height;
	baseline["italic"] = usage.baseline_italic;
	baseline["weight"] = usage.baseline_weight;
	ass_font["baseline"] = std::move(baseline);

	json::Object matched;
	matched["bold"] = usage.matched.bold;
	matched["display_name"] = usage.matched.display_name;
	matched["face_index"] = usage.matched.face_index;
	matched["facename"] = usage.matched.facename;
	matched["facename_full"] = usage.matched.facename_full;
	matched["fake_bold"] = usage.matched.fake_bold;
	matched["fake_italic"] = usage.matched.fake_italic;
	matched["italic"] = usage.matched.italic;
	matched["match_status"] = MatchStatusName(usage.matched.match_status);
	matched["missing_codepoints"] = CodepointsJson(usage.matched.missing_codepoints, usage.matched.missing_codepoint_names);
	matched["missing_lines"] = ToJson(usage.matched.missing_lines);
	matched["missing_text"] = usage.matched.missing_text;
	matched["names"] = ToJson(usage.matched.names);
	matched["path_source"] = usage.matched.path_source;
	matched["paths"] = ToJson(usage.matched.paths);
	matched["requested_weight"] = usage.matched.requested_weight;
	matched["weight"] = usage.matched.weight;
	if (usage.matched.has_backend_requested_weight)
		matched["backend_requested_weight"] = usage.matched.backend_requested_weight;
	if (!usage.matched.realized_role.empty())
		matched["realized_role"] = usage.matched.realized_role;
	if (!usage.matched.realized_status.empty())
		matched["realized_status"] = usage.matched.realized_status;
	if (usage.matched.implicit_variant_fallback)
		matched["implicit_variant_fallback"] = true;
	if (usage.matched.noncanonical_variant)
		matched["noncanonical_variant"] = true;
	json::Object match_analysis;
	match_analysis["ambiguous"] = usage.match_ambiguous;
	match_analysis["candidates"] = MatchCandidatesJson(usage.match_candidates);
	match_analysis["score_order"] = "lower_is_better";
	if (!usage.match_candidates.empty())
		matched["match_analysis"] = std::move(match_analysis);

	json::Object object;
	object["ass_font"] = std::move(ass_font);
	object["codepoints"] = CodepointsJson(usage.codepoints, usage.codepoint_names);
	object["lines"] = ToJson(usage.lines);
	object["matched_font"] = std::move(matched);
	object["override_lines"] = ToJson(usage.override_lines);
	object["styles"] = ToJson(usage.styles);
	return object;
}

template<typename T>
json::Array ToJsonArray(std::vector<T> const& values) {
	json::Array array;
	array.reserve(values.size());
	for (auto const& value : values)
		array.emplace_back(ToJson(value));
	return array;
}

json::Object ReportJson(JsonFileReport const& report) {
	auto diagnostics = BuildDiagnostics(report);
	json::Object object;
	object["matcher"] = report.context.matcher;
	object["provider"] = report.context.provider;
	object["diagnostics"] = DiagnosticsJson(diagnostics);
	object["error"] = report.error;
	object["events"] = ToJsonArray(report.context.events);
	object["font_usage"] = ToJsonArray(report.context.usages);
	object["input"] = report.input;
	object["ok"] = report.exit_result == AEGISUB_FONTCOLLECTOR_OK;
	object["operation_result"] = report.result;
	object["result"] = report.exit_result;
	object["summary"] = SummaryJson(report.summary);
	return object;
}

void WriteJsonReports(std::ostream& out,
                      std::vector<JsonFileReport> const& reports,
                      int result,
                      std::string const& provider,
	                  std::string const& matcher,
                      std::vector<JsonDiagnostic> extra_diagnostics = {}) {
	auto summary = AggregateSummary(reports);
	auto diagnostics = std::move(extra_diagnostics);
	json::Array files;
	files.reserve(reports.size());
	for (auto const& report : reports) {
		files.emplace_back(ReportJson(report));
		for (auto diagnostic : BuildDiagnostics(report))
			diagnostics.push_back(std::move(diagnostic));
	}

	json::Object root;
	root["matcher"] = matcher;
	root["provider"] = provider;
	root["diagnostics"] = DiagnosticsJson(diagnostics);
	root["files"] = std::move(files);
	root["ok"] = result == AEGISUB_FONTCOLLECTOR_OK;
	root["result"] = result;
	root["schema_version"] = 2;
	root["summary"] = SummaryJson(summary);
	agi::JsonWriter::Write(root, out);
	out << "\n";
}

json::Object NormalizationSummaryJson(AegisubFontNameNormalizationSummary const& summary) {
	json::Object object;
	object["catalog_available"] = summary.catalog_available != 0;
	object["finding_count"] = JsonInt(summary.finding_count);
	object["safe_change_count"] = JsonInt(summary.safe_change_count);
	object["scanned_name_count"] = JsonInt(summary.scanned_name_count);
	object["unsafe_finding_count"] = JsonInt(summary.unsafe_finding_count);
	return object;
}

json::Object NormalizationChangeJson(NormalizationChange const& change) {
	json::Object source;
	source["comment"] = change.comment;
	source["kind"] = NormalizationSourceKindName(change.source_kind);
	source["line"] = change.line;
	source["override_index"] = JsonInt(change.override_index);
	source["style"] = change.style;

	json::Object object;
	object["current_name"] = change.current_name;
	object["match"] = FontFamilyMatchKindName(change.match_kind);
	object["reason"] = change.reason_code;
	object["recommended_name"] = change.recommended_name;
	object["safe_to_apply"] = change.safe_to_apply;
	object["source"] = std::move(source);
	return object;
}

json::Object NormalizationReportJson(NormalizationFileReport const& report) {
	json::Array changes;
	changes.reserve(report.changes.size());
	for (auto const& change : report.changes)
		changes.emplace_back(NormalizationChangeJson(change));

	json::Object object;
	object["changes"] = std::move(changes);
	object["error"] = report.error;
	object["input"] = report.input;
	object["ok"] = report.result == AEGISUB_FONTCOLLECTOR_OK;
	object["result"] = report.result;
	object["summary"] = NormalizationSummaryJson(report.summary);
	return object;
}

void WriteNormalizationJson(
	std::ostream& out,
	std::vector<NormalizationFileReport> const& reports,
	AegisubFontNameNormalizationTarget target,
	int result)
{
	json::Array files;
	files.reserve(reports.size());
	std::size_t scanned = 0;
	std::size_t findings = 0;
	std::size_t safe = 0;
	std::size_t unsafe = 0;
	// Root catalog_available is true only when every successfully analyzed
	// file reported a usable family catalog (batch shares one snapshot).
	bool saw_ok_report = false;
	bool catalog_available = true;
	for (auto const& report : reports) {
		files.emplace_back(NormalizationReportJson(report));
		scanned += report.summary.scanned_name_count;
		findings += report.summary.finding_count;
		safe += report.summary.safe_change_count;
		unsafe += report.summary.unsafe_finding_count;
		if (report.result == AEGISUB_FONTCOLLECTOR_OK) {
			saw_ok_report = true;
			if (!report.summary.catalog_available)
				catalog_available = false;
		}
	}
	if (!saw_ok_report)
		catalog_available = false;

	json::Object summary;
	summary["catalog_available"] = catalog_available;
	summary["finding_count"] = JsonInt(findings);
	summary["safe_change_count"] = JsonInt(safe);
	summary["scanned_name_count"] = JsonInt(scanned);
	summary["unsafe_finding_count"] = JsonInt(unsafe);

	json::Object root;
	root["command"] = "normalize";
	root["files"] = std::move(files);
	root["ok"] = result == AEGISUB_FONTCOLLECTOR_OK;
	root["result"] = result;
	root["schema_version"] = 2;
	root["summary"] = std::move(summary);
	root["target"] = NormalizationTargetName(target);
	agi::JsonWriter::Write(root, out);
	out << "\n";
}

std::string NormalizationLocation(NormalizationChange const& change) {
	std::ostringstream out;
	if (change.source_kind == AEGISUB_FONT_NAME_SOURCE_STYLE) {
		out << "style '" << change.style << "'";
		if (change.line > 0)
			out << " (line " << change.line << ")";
	}
	else {
		out << (change.comment ? "comment" : "dialogue") << " line " << change.line;
		out << " (\\fn #" << change.override_index + 1 << ")";
	}
	return out.str();
}

void PrintNormalizationReport(NormalizationFileReport const& report, bool show_header, bool details) {
	if (show_header)
		std::cout << report.input << ":\n";
	if (report.result != AEGISUB_FONTCOLLECTOR_OK) {
		std::cout << "ERROR: " << report.input;
		if (!report.error.empty())
			std::cout << ": " << report.error;
		std::cout << "\n";
		return;
	}

	for (auto const& change : report.changes) {
		std::cout << (change.safe_to_apply ? "CHANGE" : "ISSUE") << ": ";
		std::cout << NormalizationLocation(change) << ": '" << change.current_name << "'";
		if (!change.recommended_name.empty())
			std::cout << " -> '" << change.recommended_name << "'";
		std::cout << " [" << change.reason_code << "]";
		if (details)
			std::cout << " match=" << FontFamilyMatchKindName(change.match_kind);
		std::cout << "\n";
	}

	if (report.changes.empty())
		std::cout << "OK: " << report.input << ": no font name changes\n";
	else
		std::cout << "Summary: " << report.summary.safe_change_count << " safe changes, "
		          << report.summary.unsafe_finding_count << " issues, "
		          << report.summary.scanned_name_count << " names scanned\n";
}

int RunNormalization(aegisub::fontcollector_subcommand::Options const& options) {
	auto inputs = ExpandInputs(options.inputs, options.recursive);
	auto encodings = ResolveCliEncodings(inputs, options.encoding);
	auto target = options.normalization_target == "english"
		? AEGISUB_FONT_NAME_TARGET_ENGLISH_WIN32
		: AEGISUB_FONT_NAME_TARGET_LOCALIZED;
	if (inputs.empty()) {
		if (options.json)
			WriteNormalizationJson(std::cout, {}, target, AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT);
		else
			std::cerr << "fontcollector normalize failed: no ASS/SSA files found\n";
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	std::vector<NormalizationFileReport> reports(inputs.size());
	std::vector<AegisubFontNameNormalizationRequest> requests(inputs.size());
	std::vector<AegisubFontNameNormalizationBatchItem> items(inputs.size());
	std::vector<AegisubFontNameNormalizationBatchItem *> item_pointers(inputs.size());
	std::vector<std::array<char, 4096>> errors(inputs.size());
	for (size_t i = 0; i < inputs.size(); ++i) {
		auto& report = reports[i];
		report.input = inputs[i];
		report.summary.struct_size = sizeof(report.summary);

		auto& request = requests[i];
		request.struct_size = sizeof(request);
		request.input_path = inputs[i].c_str();
		request.encoding = encodings[i].c_str();
		request.target = target;

		auto& item = items[i];
		item.struct_size = sizeof(item);
		item.request = &request;
		item.callback = &CollectNormalizationChange;
		item.user_data = &report.changes;
		item.summary = &report.summary;
		item.error_buffer = errors[i].data();
		item.error_buffer_size = errors[i].size();
		item_pointers[i] = &item;
	}

	std::array<char, 4096> batch_error = {};
	auto batch_result = aegisub_fontcollector_build_normalization_plan_batch(
		item_pointers.data(), item_pointers.size(), batch_error.data(), batch_error.size());
	if (batch_result != AEGISUB_FONTCOLLECTOR_OK) {
		for (auto& report : reports) {
			report.result = batch_result;
			report.error = batch_error.data();
		}
		if (options.json)
			WriteNormalizationJson(std::cout, reports, target, batch_result);
		else
			std::cerr << "fontcollector normalize failed: " << batch_error.data() << "\n";
		return batch_result;
	}

	int exit_code = AEGISUB_FONTCOLLECTOR_OK;
	for (size_t i = 0; i < reports.size(); ++i) {
		reports[i].result = items[i].result;
		reports[i].error = errors[i].data();
		exit_code = std::max(exit_code, reports[i].result);
	}

	if (options.json)
		WriteNormalizationJson(std::cout, reports, target, exit_code);
	else {
		for (size_t i = 0; i < reports.size(); ++i) {
			if (i)
				std::cout << "\n";
			PrintNormalizationReport(reports[i], reports.size() > 1, options.details);
		}
	}
	return exit_code;
}

AegisubFontCollectorMatcher ParseMatcherOption(std::string const& matcher) {
	return matcher == "libass"
		? AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS
		: AEGISUB_FONTCOLLECTOR_MATCHER_PLATFORM;
}

int RunInventory(aegisub::fontcollector_subcommand::Options const& options) {
	font_usage_inventory::Options request;
	for (auto const& input : options.inputs)
		request.inputs.push_back(Utf8ToPath(input));
	request.recursive = options.recursive;
	request.low_frequency_threshold = options.low_frequency_threshold;
	request.encoding = options.encoding;
	request.matcher = options.matcher == "libass"
						  ? FontCollectorMatcher::Libass
						  : FontCollectorMatcher::Platform;
	request.provider_options.include_system_fonts = !options.exclude_system_fonts;
	auto write_result = [](font_usage_inventory::Result const& result) {
		agi::JsonWriter::Write(font_usage_inventory::ToJson(result), std::cout);
		std::cout << '\n';
		return result.result;
	};
	auto fail_invalid_option = [&](std::string message) {
		return write_result(font_usage_inventory::InvalidOptions(request, std::move(message)));
	};
	if (options.matcher != "platform" && options.matcher != "libass")
		return fail_invalid_option("invalid font matcher");
	if (request.matcher != FontCollectorMatcher::Libass &&
		(!options.additional_fonts.empty() || !options.additional_fonts_recursive.empty() ||
		 options.exclude_system_fonts)) {
		return fail_invalid_option("private font options require --matcher libass");
	}
	request.provider_options.additional_font_files = ExpandFontInputs(options.additional_fonts, false);
	auto recursive_fonts = ExpandFontInputs(options.additional_fonts_recursive, true);
	for (auto& path : recursive_fonts)
		AddUnique(request.provider_options.additional_font_files, std::move(path));
	if ((!options.additional_fonts.empty() || !options.additional_fonts_recursive.empty()) &&
		request.provider_options.additional_font_files.empty()) {
		return fail_invalid_option("no supported files found in additional font paths");
	}
	if (options.exclude_system_fonts && request.provider_options.additional_font_files.empty())
		return fail_invalid_option("--exclude-system-fonts requires --additional-fonts");
	return write_result(font_usage_inventory::Scan(request));
}

int RunFontCollector(aegisub::fontcollector_subcommand::Options const& options) {
	auto requested_matcher = ParseMatcherOption(options.matcher);
	auto provider_name = ProviderName(requested_matcher);
	auto matcher_name = MatcherName(requested_matcher);
	auto fail_invalid_option = [&](std::string message) {
		if (!options.json)
			std::cerr << "fontcollector failed: " << message << "\n";
		else {
			auto diagnostic = MakeDiagnostic("invalid_argument", "error", std::string(), message);
			diagnostic.has_result = true;
			diagnostic.result = AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
			WriteJsonReports(
				std::cout,
				{},
				AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT,
				provider_name,
				matcher_name,
				{std::move(diagnostic)});
		}
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	};
	if (requested_matcher != AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS &&
	    (!options.additional_fonts.empty() || !options.additional_fonts_recursive.empty() ||
	     options.exclude_system_fonts)) {
		return fail_invalid_option("private font options require --matcher libass");
	}
	auto additional_font_files = ExpandFontInputs(options.additional_fonts, false);
	auto recursive_font_files = ExpandFontInputs(options.additional_fonts_recursive, true);
	for (auto& path : recursive_font_files)
		AddUnique(additional_font_files, std::move(path));
	if (requested_matcher == AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS &&
	    (!options.additional_fonts.empty() || !options.additional_fonts_recursive.empty()) &&
	    additional_font_files.empty()) {
		return fail_invalid_option("no supported files found in additional font paths");
	}
	if (requested_matcher == AEGISUB_FONTCOLLECTOR_MATCHER_LIBASS &&
	    options.exclude_system_fonts &&
	    additional_font_files.empty()) {
		return fail_invalid_option("--exclude-system-fonts requires --additional-fonts");
	}
	auto inputs = ExpandInputs(options.inputs, options.recursive);
	auto encodings = ResolveCliEncodings(inputs, options.encoding);
	if (inputs.empty()) {
		if (!options.json)
			std::cerr << "fontcollector failed: no ASS/SSA files found\n";
		else {
			auto diagnostic = MakeDiagnostic("invalid_argument", "error", std::string(), "no ASS/SSA files found");
			diagnostic.has_result = true;
			diagnostic.result = AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
			WriteJsonReports(
				std::cout,
				{},
				AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT,
				provider_name,
				matcher_name,
				{std::move(diagnostic)});
		}
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	auto const *destination = options.destination.empty() ? nullptr : options.destination.c_str();
	auto const mode = options.operation == aegisub::fontcollector_subcommand::Operation::Collect
		? (options.copy_to_script_directory
			? AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER
			: AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER)
		: AEGISUB_FONTCOLLECTOR_MODE_CHECK;
	auto const list = options.operation == aegisub::fontcollector_subcommand::Operation::List;
	auto const strict = options.strict ||
		options.operation == aegisub::fontcollector_subcommand::Operation::Validate;

	std::array<char, 4096> session_error = {};
	AegisubFontCollectorSession *session = nullptr;
	JsonContext session_context;
	session_context.provider = provider_name;
	session_context.matcher = matcher_name;
	std::vector<char const *> additional_font_file_pointers;
	additional_font_file_pointers.reserve(additional_font_files.size());
	for (auto const& path : additional_font_files)
		additional_font_file_pointers.push_back(path.c_str());
	AegisubFontCollectorSessionOptions session_options = {};
	session_options.struct_size = sizeof(session_options);
	session_options.matcher = requested_matcher;
	session_options.additional_font_files = additional_font_file_pointers.empty()
		? nullptr
		: additional_font_file_pointers.data();
	session_options.additional_font_file_count = additional_font_file_pointers.size();
	session_options.include_system_fonts = !options.exclude_system_fonts;
	session_options.collect_match_candidates = options.details || options.json;
	int session_result = aegisub_fontcollector_session_create_with_options(
		&session_options,
		&CollectJsonEvent,
		static_cast<void *>(&session_context),
		&session,
		session_error.data(),
		session_error.size());
	std::unique_ptr<AegisubFontCollectorSession, decltype(&aegisub_fontcollector_session_destroy)>
		session_guard(session, &aegisub_fontcollector_session_destroy);

	if (session_result != AEGISUB_FONTCOLLECTOR_OK) {
		if (!options.json) {
			std::cerr << "fontcollector failed";
			if (session_error[0])
				std::cerr << ": " << session_error.data();
			std::cerr << "\n";
		}
		else {
			auto diagnostic = MakeDiagnostic("session_error", "error", std::string(), session_error.data());
			diagnostic.has_result = true;
			diagnostic.result = session_result;
			WriteJsonReports(
				std::cout,
				{},
				session_result,
				provider_name,
				matcher_name,
				{std::move(diagnostic)});
		}
		return session_result;
	}

	std::vector<JsonFileReport> json_reports;
	json_reports.resize(inputs.size());
	std::vector<AegisubFontCollectorRequest> requests(inputs.size());
	std::vector<AegisubFontCollectorBatchItem> batch_items(inputs.size());
	std::vector<std::array<char, 4096>> errors(inputs.size());
	std::vector<AegisubFontCollectorSummary> summaries(inputs.size());

	for (size_t i = 0; i < inputs.size(); ++i) {
		auto& report = json_reports[i];
		report.input = inputs[i];
		report.context.provider = provider_name;
		report.context.matcher = matcher_name;
		if (i == 0)
			report.context.events = session_context.events;

		auto& request = requests[i];
		request.input_path = inputs[i].c_str();
		request.destination_path = destination;
		request.encoding = encodings[i].c_str();
		request.mode = mode;
		auto& item = batch_items[i];
		item.request = request;
		item.event_callback = &CollectJsonEvent;
		item.event_user_data = static_cast<void *>(&report.context);
		item.usage_callback = (options.json || options.details || list) ? &CollectJsonUsage : nullptr;
		item.usage_user_data = static_cast<void *>(&report.context);
		item.summary = &summaries[i];
		item.error_buffer = errors[i].data();
		item.error_buffer_size = errors[i].size();
		item.result = AEGISUB_FONTCOLLECTOR_OK;
	}

	std::array<char, 4096> batch_error = {};
	int batch_result = aegisub_fontcollector_session_collect_batch(
		session,
		batch_items.empty() ? nullptr : batch_items.data(),
		batch_items.size(),
		batch_error.data(),
		batch_error.size());
	if (batch_result != AEGISUB_FONTCOLLECTOR_OK) {
		if (!options.json) {
			std::cerr << "fontcollector failed";
			if (batch_error[0])
				std::cerr << ": " << batch_error.data();
			std::cerr << "\n";
		}
		else {
			auto diagnostic = MakeDiagnostic("batch_error", "error", std::string(), batch_error.data());
			diagnostic.has_result = true;
			diagnostic.result = batch_result;
			WriteJsonReports(
				std::cout,
				json_reports,
				batch_result,
				provider_name,
				matcher_name,
				{std::move(diagnostic)});
		}
		return batch_result;
	}

	int exit_code = 0;
	for (size_t i = 0; i < json_reports.size(); ++i) {
		auto& report = json_reports[i];
		report.result = batch_items[i].result;
		report.error = errors[i].data();
		report.summary = summaries[i];
		report.exit_result = report.result;

		if (report.result != AEGISUB_FONTCOLLECTOR_OK)
			report.exit_result = report.result;
		else if (strict && HasStrictFindings(report.summary))
			report.exit_result = ValidationFailedExitCode;

		exit_code = std::max(exit_code, report.exit_result);
	}

	if (list || options.details)
		PopulateMatchedFontNames(json_reports);

	if (!options.json && !list) {
		for (size_t i = 0; i < json_reports.size(); ++i) {
			if (i && inputs.size() > 1)
				std::cout << "\n";
			PrintStoredReport(json_reports[i], inputs.size() > 1, options.details);
		}
	}
	else if (options.json)
		WriteJsonReports(std::cout, json_reports, exit_code,
		                 provider_name, matcher_name);
	else if (list)
		PrintListReports(json_reports, options.details, options.quiet);

	return exit_code;
}
}

namespace aegisub::fontcollector_subcommand {

int Run(Options const& options) {
	switch (options.operation) {
		case Operation::Check:
		case Operation::Collect:
		case Operation::Validate:
		case Operation::List:
			return RunFontCollector(options);
		case Operation::Normalize:
			return RunNormalization(options);
		case Operation::Inventory:
			return RunInventory(options);
	}

	std::cerr << "fontcollector failed: invalid operation\n";
	return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
}

}
