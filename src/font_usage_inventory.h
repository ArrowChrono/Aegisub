#pragma once

#include "font_collector_backend.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace font_usage_inventory {

struct Options {
	std::vector<agi::fs::path> inputs;
	bool recursive = false;
	int low_frequency_threshold = 5;
	std::string encoding;
	FontCollectorMatcher matcher = FontCollectorMatcher::Platform;
	FontProviderOptions provider_options;
};

struct Diagnostic {
	std::string code;
	std::string severity;
	std::string message;
	std::string input;
	std::vector<int> lines;
	std::string ass_name;
	std::optional<int> requested_weight;
	std::optional<bool> requested_italic;
	std::vector<uint32_t> codepoints;
};

struct Reason {
	std::string code;
	std::string message;
};

struct Source {
	std::string input;
	uint64_t usage_count = 0;
};

struct Request {
	std::string input;
	std::string ass_name;
	int ass_bold = 0;
	int effective_weight = 400;
	bool italic = false;
	int charset = 1;
	double height = 0;
	std::string raw_bold_tag;
	std::string raw_italic_tag;
	std::string raw_charset_tag;
	std::string raw_height_tag;
	bool explicit_family = false;
	bool explicit_bold = false;
	bool explicit_italic = false;
	bool explicit_charset = false;
	bool explicit_height = false;
	std::string baseline_name;
	int baseline_weight = 400;
	bool baseline_italic = false;
	int baseline_charset = 1;
	double baseline_height = 0;
	std::vector<std::string> styles;
	std::vector<int> lines;
	std::vector<int> override_lines;
	std::string matched_name;
	std::string matched_full_name;
	int matched_weight = 0;
	bool matched_bold = false;
	bool matched_italic = false;
	int face_index = -1;
	std::string path_source;
	std::vector<std::string> paths;
	bool memory_only = false;
};

struct Row {
	std::string key;
	bool selection_reliable = true;
	std::vector<std::string> ass_names;
	std::optional<std::string> localized_name;
	std::optional<std::string> english_name;
	std::string display_name;
	uint64_t usage_count = 0;
	bool is_low_usage = false;
	bool missing_font = false;
	std::optional<std::vector<uint32_t>> missing_codepoints;
	std::vector<Diagnostic> warnings;
	std::string display_state = "normal";
	std::vector<Reason> reasons;
	std::vector<Source> sources;
	std::vector<Request> requests;
};

struct Input {
	std::string path;
	std::string kind;
};

struct File {
	std::string input;
	std::string status;
};

struct Result {
	std::string locale;
	std::string matcher;
	std::string provider;
	bool recursive = false;
	int low_frequency_threshold = 5;
	bool complete = true;
	int result = 0;
	uint64_t catalog_generation = 0;
	std::vector<Input> inputs;
	std::vector<File> files;
	std::vector<agi::fs::path> watch_directories;
	std::vector<Row> rows;
	std::vector<Diagnostic> diagnostics;
};

Result Scan(Options const& options);
Result InvalidOptions(Options const& options, std::string message);
void RecomputeThreshold(Result& result, int threshold);
json::Object ToJson(Result const& result);
std::string FormatMissingCodepoints(std::span<uint32_t const> values);
std::string SystemDisplayLocale();

}
