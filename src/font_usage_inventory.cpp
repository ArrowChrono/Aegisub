#include "font_usage_inventory.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_parser.h"
#include "font_collector_core.h"
#include "font_collector_unicode.h"
#include "font_family_catalog.h"
#include "font_family_catalog_cache.h"
#include "font_file_lister.h"
#include "fontcollector_cli_encoding.h"
#include "text_file_reader.h"

#include <libaegisub/charset.h>
#include <libaegisub/charset_conv.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#ifdef _WIN32
#include "font_file_lister_dwrite.h"
#include "font_matching_libass.h"
#include "gdi_font_resolver.h"
#include <libaegisub/charset_conv_win.h>
#include <dwrite.h>
#endif

namespace font_usage_inventory {
namespace {

std::string Fold(std::string value) {
#ifdef _WIN32
	auto const wide = agi::charset::ConvertW(value);
	int const size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
								   wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
	if (size > 0) {
		std::wstring folded(size, L'\0');
		if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
						  wide.data(), static_cast<int>(wide.size()), folded.data(), size,
						  nullptr, nullptr, 0) > 0)
			return agi::charset::ConvertW(folded);
	}
#endif
	for (auto& ch : value)
		if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
	return value;
}

std::string PathKey(agi::fs::path const& path) {
	std::error_code ec;
	auto normalized = std::filesystem::weakly_canonical(path, ec);
	if (ec) normalized = std::filesystem::absolute(path, ec).lexically_normal();
	auto key = agi::fs::PathToGenericString(normalized);
#ifdef _WIN32
	return Fold(std::move(key));
#else
	return key;
#endif
}

agi::fs::path AbsolutePath(agi::fs::path const& path) {
	std::error_code ec;
	auto absolute = std::filesystem::absolute(path, ec);
	return (ec ? path : absolute).lexically_normal();
}

std::string WatchKey(agi::fs::path const& path) {
	auto text = agi::fs::PathToGenericString(path);
#ifdef _WIN32
	return Fold(std::move(text));
#else
	return text;
#endif
}

void AddWatchAncestor(agi::fs::path path, Result& result) {
	path = AbsolutePath(path);
	while (!path.empty()) {
		std::error_code ec;
		if (std::filesystem::is_directory(path, ec) && !ec) {
			result.watch_directories.push_back(std::move(path));
			return;
		}
		auto parent = path.parent_path();
		if (parent == path) return;
		path = std::move(parent);
	}
}

void FinalizeWatchPlan(Result& result) {
	for (auto& path : result.watch_directories) path = AbsolutePath(path);
	std::ranges::sort(result.watch_directories, [](auto const& a, auto const& b) {
		return std::pair(WatchKey(a), agi::fs::PathToString(a)) < std::pair(WatchKey(b), agi::fs::PathToString(b));
	});
	result.watch_directories.erase(std::ranges::unique(result.watch_directories, [](auto const& a, auto const& b) {
									   return WatchKey(a) == WatchKey(b);
								   }).begin(),
								   result.watch_directories.end());
}

std::string Token(std::string_view value) {
	return std::to_string(value.size()) + ":" + std::string(value);
}

template <class T>
void SortUnique(std::vector<T>& values) {
	std::ranges::sort(values);
	values.erase(std::ranges::unique(values).begin(), values.end());
}

std::string NormalizeLocale(std::string value) {
	std::ranges::replace(value, '_', '-');
	return Fold(std::move(value));
}

std::string PrimaryLanguage(std::string const& value) {
	return value.substr(0, value.find('-'));
}

std::optional<std::string> PickName(std::vector<FontFamilyName> const& names,
									std::string const& locale, bool english) {
	std::vector<std::pair<int, std::string>> candidates;
	for (auto const& name : names) {
		if (name.kind != FontFamilyNameKind::Win32Family || name.value.empty())
			continue;
		auto const language = NormalizeLocale(name.locale);
		int rank = 3;
		if (english) {
			if (PrimaryLanguage(language) != "en") continue;
			rank = language == "en-us" ? 0 : 1;
		}
		else if (language == NormalizeLocale(locale))
			rank = 0;
		else if (!language.empty() && PrimaryLanguage(language) == PrimaryLanguage(NormalizeLocale(locale)))
			rank = 1;
		else
			continue;
		candidates.emplace_back(rank, name.value);
	}
	if (candidates.empty()) return {};
	std::ranges::sort(candidates);
	return candidates.front().second;
}

std::string ProviderName(FontCollectorMatcher matcher) {
#ifdef _WIN32
	return matcher == FontCollectorMatcher::Libass ? "directwrite" : "gdi-dwrite";
#elif defined(__APPLE__)
	return matcher == FontCollectorMatcher::Libass ? "fontconfig" : "coretext";
#else
	return "fontconfig";
#endif
}

Result BaseResult(Options const& options) {
	Result result;
	result.locale = SystemDisplayLocale();
	result.matcher = options.matcher == FontCollectorMatcher::Libass ? "libass" : "platform";
	result.provider = ProviderName(options.matcher);
	result.recursive = options.recursive;
	result.low_frequency_threshold = options.low_frequency_threshold;
	result.catalog_generation = font_family_catalog_cache::GetGeneration();
	return result;
}

void Fail(Result& result, int code, std::string type, std::string input, std::string message) {
	result.complete = false;
	result.result = std::max(result.result, code);
	Diagnostic diagnostic;
	diagnostic.code = std::move(type);
	diagnostic.severity = "error";
	diagnostic.input = std::move(input);
	diagnostic.message = std::move(message);
	result.diagnostics.push_back(std::move(diagnostic));
}

bool IsAss(agi::fs::path const& path) {
	return Fold(agi::fs::PathToString(path.extension())) == ".ass";
}

bool IsLinkedDirectory(std::filesystem::directory_entry const& entry, std::error_code& ec) {
	if (entry.is_symlink(ec) || ec) return true;
#ifdef _WIN32
	auto const attributes = GetFileAttributesW(entry.path().c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		ec = std::error_code(GetLastError(), std::system_category());
		return true;
	}
	return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
	return false;
#endif
}

void ExpandDirectory(Options const& options, agi::fs::path const& root, Result& result,
					 std::map<std::string, agi::fs::path>& files, std::set<std::string>& directories) {
	std::vector<agi::fs::path> pending{root};
	while (!pending.empty()) {
		auto directory = std::move(pending.back());
		pending.pop_back();
		if (!directories.insert(PathKey(directory)).second) continue;
		result.watch_directories.push_back(directory);
		std::error_code ec;
		std::filesystem::directory_iterator it(directory, ec), end;
		if (ec) {
			Fail(result, 5, "file_error", agi::fs::PathToString(directory), ec.message());
			continue;
		}
		for (; it != end; it.increment(ec)) {
			if (ec) break;
			auto const& entry = *it;
			if (entry.is_directory(ec)) {
				if (options.recursive && !IsLinkedDirectory(entry, ec))
					pending.push_back(entry.path());
			}
			else if (!ec && entry.is_regular_file(ec) && IsAss(entry.path())) {
				auto const key = PathKey(entry.path());
				auto [found, inserted] = files.emplace(key, entry.path());
				if (!inserted && agi::fs::PathToString(entry.path()) < agi::fs::PathToString(found->second))
					found->second = entry.path();
			}
			if (ec) {
				Fail(result, 5, "file_error", agi::fs::PathToString(entry.path()), ec.message());
				ec.clear();
			}
		}
		if (ec) Fail(result, 5, "file_error", agi::fs::PathToString(directory), ec.message());
	}
}

std::vector<agi::fs::path> ExpandInputs(Options const& options, Result& result) {
	std::map<std::string, agi::fs::path> files;
	std::set<std::string> directories;
	auto inputs = options.inputs;
	std::ranges::sort(inputs, [](auto const& a, auto const& b) {
		return agi::fs::PathToString(a) < agi::fs::PathToString(b);
	});
	for (auto const& input : inputs) {
		auto const text = agi::fs::PathToString(input);
		AddWatchAncestor(AbsolutePath(input).parent_path(), result);
		std::error_code ec;
		auto const status = std::filesystem::status(input, ec);
		if (ec || !std::filesystem::exists(status)) {
			result.inputs.push_back({.path = text, .kind = "invalid"});
			if (IsAss(input)) result.files.push_back({.input = text, .status = "error"});
			Fail(result, 5, "file_error", text, ec ? ec.message() : "input does not exist");
		}
		else if (std::filesystem::is_directory(status)) {
			result.inputs.push_back({.path = text, .kind = "directory"});
			ExpandDirectory(options, input, result, files, directories);
		}
		else if (std::filesystem::is_regular_file(status) && IsAss(input)) {
			result.inputs.push_back({.path = text, .kind = "file"});
			files.try_emplace(PathKey(input), input);
		}
		else {
			result.inputs.push_back({.path = text, .kind = "invalid"});
			Fail(result, 1, "invalid_argument", text, "explicit input must be an ASS file or directory");
		}
	}
	std::vector<agi::fs::path> expanded;
	expanded.reserve(files.size());
	for (auto const& [key, file] : files) expanded.push_back(file);
	FinalizeWatchPlan(result);
	return expanded;
}

std::unique_ptr<AssFile> ReadFile(agi::fs::path const& path, std::string encoding) {
	if (encoding.empty()) encoding = aegisub::fontcollector_cli::PreferredAutomaticEncoding(path);
	if (encoding.empty()) encoding = agi::charset::Detect(path);
	auto file = std::make_unique<AssFile>();
	TextFileReader reader(path, encoding);
	AssParser parser(file.get(), 1);
	int line_number = 0;
	while (reader.HasMoreLines()) {
		++line_number;
		auto const line = reader.ReadLineFromFile();
		auto *previous = file->Events.empty() ? nullptr : &file->Events.back();
		parser.AddLine(line);
		if (!file->Events.empty() && &file->Events.back() != previous)
			file->Events.back().Row = line_number - 1;
	}
	return file;
}

struct Identity {
	std::string key;
	std::optional<std::string> localized;
	std::optional<std::string> english;
	std::string fallback;
	bool reliable = true;
};

std::string PhysicalKey(FontCollectorMatchedFont const& font) {
	if (!font.primary_path.empty())
		return Token(PathKey(font.primary_path)) + ":" + std::to_string(font.face_index);
	if (font.primary_memory_data && !font.primary_memory_data->empty()) {
		uint64_t hash = 14695981039346656037ull;
		for (unsigned char ch : *font.primary_memory_data) hash = (hash ^ ch) * 1099511628211ull;
		return "memory:" + std::to_string(hash) + ":" + std::to_string(font.face_index);
	}
	std::vector<std::string> sources;
	sources.reserve(font.paths.size());
	for (auto const& path : font.paths) sources.push_back(PathKey(path));
	SortUnique(sources);
	std::string result;
	for (auto const& source : sources) result += Token(source);
	result += ":" + std::to_string(font.face_index);
	for (auto const& memory : font.memory_fonts) {
		uint64_t hash = 14695981039346656037ull;
		if (memory.data)
			for (unsigned char ch : *memory.data) hash = (hash ^ ch) * 1099511628211ull;
		result += ":memory:" + std::to_string(hash);
	}
	return result;
}

class IdentityResolver {
	Options const& options;
	std::string locale;
	std::shared_ptr<FontFamilyCatalog const> catalog;
	std::map<std::tuple<FontFamilyId, int, int>, Identity> installed;
#ifdef _WIN32
	std::unique_ptr<GdiFontResolver> gdi;
	std::unique_ptr<DWriteBridge> bridge;
	std::vector<LibassFontFace> faces;
	std::vector<IDWriteFontFace *> dwrite_faces;
#endif

	public:
	IdentityResolver(Options const& options, std::string locale,
					 std::shared_ptr<FontFamilyCatalog const> catalog)
		: options(options), locale(std::move(locale)), catalog(std::move(catalog)) {
#ifdef _WIN32
		if (options.matcher == FontCollectorMatcher::Platform)
			gdi = std::make_unique<GdiFontResolver>();
		else {
			bridge = std::make_unique<DWriteBridge>();
			std::string error;
			bridge->BuildFontCatalog(faces, dwrite_faces,
									 options.provider_options.additional_font_files,
									 options.provider_options.include_system_fonts, error);
		}
#endif
	}

	~IdentityResolver() {
#ifdef _WIN32
		for (auto *face : dwrite_faces) face->Release();
#endif
	}

	Identity Resolve(FontCollectorAssFontUsage const& usage) {
		auto const& font = usage.matched;
		Identity result;
		result.fallback = font.facename.empty() ? usage.ass_facename : font.facename;
		if (font.paths.empty() && font.memory_fonts.empty()) {
			result.fallback = usage.ass_facename;
			result.key = "missing:" + Token(usage.ass_facename);
			return result;
		}
		if (options.matcher == FontCollectorMatcher::Platform && catalog && !font.match_ambiguous) {
			auto const resolution = catalog->Resolve(font.facename);
			if (resolution.family) {
				auto const *family = catalog->Find(*resolution.family);
				if (family) {
					int height = 0;
#ifdef _WIN32
					height = QuantizeAssHeightForGdiProbe(usage.ass_height);
#endif
					auto const profile_key = std::tuple(family->id, usage.ass_charset, height);
					auto found = installed.find(profile_key);
					if (found == installed.end()) {
						Identity profile;
						profile.localized = PickName(family->names, locale, false);
						profile.english = PickName(family->names, locale, true);
						if (!profile.english && !family->english_win32_family_name.empty()) profile.english = family->english_win32_family_name;
						profile.fallback = family->localized_family_name;
						std::vector<std::string> names;
						for (auto const& name : family->names)
							if (name.kind == FontFamilyNameKind::Win32Family || name.kind == FontFamilyNameKind::PlatformAlias)
								names.push_back(name.value);
						SortUnique(names);
						profile.key = "family:";
						for (auto const& name : names) profile.key += Token(name);
#ifdef _WIN32
						for (auto const& [weight, italic] : std::array<std::pair<int, bool>, 4>{{{FW_NORMAL, false}, {FW_BOLD, false}, {FW_NORMAL, true}, {FW_BOLD, true}}}) {
							std::string source;
							auto const probe = gdi->ProbeWithSelection(font.facename, weight, italic,
																	   usage.ass_charset, height, [&](GdiFontSelectionView const& view) {
																		   if (!view.local_file_path.empty())
																			   source = Token(PathKey(agi::fs::PathFromString(std::string(view.local_file_path)))) + ":" + std::to_string(view.face_index);
																	   });
							profile.reliable &= probe.success && probe.matches_requested_family && !source.empty();
							profile.key += Token(source) + ":" + std::to_string(probe.outcome.realized_weight) + ":" + std::to_string(probe.outcome.realized_italic);
						}
#else
						profile.reliable = false;
#endif
						found = installed.emplace(profile_key, std::move(profile)).first;
					}
					result.localized = found->second.localized;
					result.english = found->second.english;
					if (found->second.reliable) return found->second;
				}
			}
		}
#ifdef _WIN32
		if (options.matcher == FontCollectorMatcher::Libass && bridge && !font.match_ambiguous) {
			std::optional<size_t> selected;
			for (size_t i = 0; i < faces.size(); ++i) {
				if (faces[i].face_index != font.face_index) continue;
				if (!font.primary_path.empty() && !faces[i].path.empty() &&
					PathKey(font.primary_path) == PathKey(agi::fs::PathFromString(faces[i].path))) {
					selected = i;
					break;
				}
			}
			if (selected) {
				auto const& face = faces[*selected];
				std::vector<FontFamilyName> names;
				for (auto const& name : bridge->GetWin32FamilyNamesFromFace(dwrite_faces[*selected]))
					names.push_back({.value = name.value, .locale = name.locale});
				result.localized = PickName(names, locale, false);
				result.english = PickName(names, locale, true);
				std::vector<std::string> aliases = face.families;
				SortUnique(aliases);
				result.key = "family:";
				for (auto const& alias : aliases) result.key += Token(alias);
				bool reliable = true;
				for (auto const& request : std::array<LibassFontRequest, 4>{{{.family = font.facename, .weight = 400, .italic = false},
																			 {.family = font.facename, .weight = 700, .italic = false},
																			 {.family = font.facename, .weight = 400, .italic = true},
																			 {.family = font.facename, .weight = 700, .italic = true}}}) {
					auto const ranking = RankLibassFamilyFaces(faces, request);
					if (!ranking.face || ranking.ambiguous) {
						reliable = false;
						break;
					}
					auto const& ranked = faces[*ranking.face];
					if (ranked.path.empty()) {
						reliable = false;
						break;
					}
					result.key += Token(PathKey(agi::fs::PathFromString(ranked.path))) + ":" + std::to_string(ranked.face_index);
				}
				if (reliable) return result;
			}
		}
#endif
		result.key = "physical:" + PhysicalKey(font) + Token(font.facename);
		if (font.match_ambiguous) {
			result.key += ":ambiguous:" + Token(usage.ass_facename) + ":" + std::to_string(usage.ass_effective_weight) + ":" + std::to_string(usage.ass_italic);
			std::vector<std::string> candidates;
			candidates.reserve(font.match_candidates.size());
			for (auto const& candidate : font.match_candidates)
				candidates.push_back(Token(candidate.path.empty() ? candidate.facename : PathKey(agi::fs::PathFromString(candidate.path))) + ":" + std::to_string(candidate.face_index));
			SortUnique(candidates);
			for (auto const& candidate : candidates) result.key += Token(candidate);
		}
		bool const has_primary = !font.primary_path.empty() || (font.primary_memory_data && !font.primary_memory_data->empty());
		result.reliable = !font.match_ambiguous &&
						  (options.matcher == FontCollectorMatcher::Libass ? has_primary : !font.paths.empty() || !font.memory_fonts.empty());
		return result;
	}
};

Diagnostic UsageDiagnostic(std::string code, std::string severity, std::string message,
						   std::string const& input, FontCollectorAssFontUsage const& usage) {
	Diagnostic diagnostic;
	diagnostic.code = std::move(code);
	diagnostic.severity = std::move(severity);
	diagnostic.message = std::move(message);
	diagnostic.input = input;
	diagnostic.lines = usage.lines;
	diagnostic.ass_name = usage.ass_facename;
	diagnostic.requested_weight = usage.ass_effective_weight;
	diagnostic.requested_italic = usage.ass_italic;
	return diagnostic;
}

Request UsageRequest(std::string const& input, FontCollectorAssFontUsage const& usage) {
	Request request;
	request.input = input;
	request.ass_name = usage.ass_facename;
	request.ass_bold = usage.ass_bold;
	request.effective_weight = usage.ass_effective_weight;
	request.italic = usage.ass_italic;
	request.charset = usage.ass_charset;
	request.height = usage.ass_height;
	request.raw_bold_tag = usage.ass_raw_bold_tag;
	request.raw_italic_tag = usage.ass_raw_italic_tag;
	request.raw_charset_tag = usage.ass_raw_charset_tag;
	request.raw_height_tag = usage.ass_raw_height_tag;
	request.explicit_family = usage.ass_has_explicit_family;
	request.explicit_bold = usage.ass_has_explicit_bold;
	request.explicit_italic = usage.ass_has_explicit_italic;
	request.explicit_charset = usage.ass_has_explicit_charset;
	request.explicit_height = usage.ass_has_explicit_height;
	request.baseline_name = usage.baseline_facename;
	request.baseline_weight = usage.baseline_weight;
	request.baseline_italic = usage.baseline_italic;
	request.baseline_charset = usage.baseline_charset;
	request.baseline_height = usage.baseline_height;
	request.styles = usage.styles;
	request.lines = usage.lines;
	request.override_lines = usage.override_lines;
	request.matched_name = usage.matched.facename;
	request.matched_full_name = usage.matched.facename_full;
	request.matched_weight = usage.matched.weight;
	request.matched_bold = usage.matched.bold;
	request.matched_italic = usage.matched.italic;
	request.face_index = usage.matched.face_index;
	request.path_source = usage.matched.path_source;
	for (auto const& path : usage.matched.paths) request.paths.push_back(agi::fs::PathToString(path));
	request.memory_only = request.paths.empty() && !usage.matched.memory_fonts.empty();
	return request;
}

std::string WarningMessage(std::string const& code) {
	if (code == "fake_bold") return "Warning: synthetic bold";
	if (code == "fake_italic") return "Warning: synthetic italic";
	if (code == "ambiguous_font_match") return "Warning: ambiguous font match";
	return "Warning: " + code;
}

template <class T>
json::Array Array(std::vector<T> const& values) {
	json::Array array;
	for (auto const& value : values) array.emplace_back(value);
	return array;
}

json::Array Codepoints(std::vector<uint32_t> const& values) {
	json::Array array;
	for (auto value : values) array.emplace_back(static_cast<json::Integer>(value));
	return array;
}

json::Object DiagnosticJson(Diagnostic const& value) {
	json::Object object;
	object["code"] = value.code;
	object["type"] = value.code;
	object["severity"] = value.severity;
	object["message"] = value.message;
	object["file"] = value.input;
	object["lines"] = Array(value.lines);
	object["ass_name"] = value.ass_name.empty() ? json::UnknownElement(json::Null()) : json::UnknownElement(value.ass_name);
	object["requested_weight"] = value.requested_weight ? json::UnknownElement(static_cast<json::Integer>(*value.requested_weight)) : json::UnknownElement(json::Null());
	object["requested_italic"] = value.requested_italic ? json::UnknownElement(*value.requested_italic) : json::UnknownElement(json::Null());
	object["codepoints"] = Codepoints(value.codepoints);
	return object;
}

json::Array DiagnosticsJson(std::vector<Diagnostic> const& values) {
	json::Array array;
	for (auto const& value : values) array.emplace_back(DiagnosticJson(value));
	return array;
}

json::Object RequestJson(Request const& request) {
	json::Object object;
	object["input"] = request.input;
	object["ass_name"] = request.ass_name;
	object["ass_bold"] = request.ass_bold;
	object["effective_weight"] = request.effective_weight;
	object["italic"] = request.italic;
	object["charset"] = request.charset;
	object["height"] = request.height;
	object["raw_bold_tag"] = request.raw_bold_tag;
	object["raw_italic_tag"] = request.raw_italic_tag;
	object["raw_charset_tag"] = request.raw_charset_tag;
	object["raw_height_tag"] = request.raw_height_tag;
	object["explicit_family"] = request.explicit_family;
	object["explicit_bold"] = request.explicit_bold;
	object["explicit_italic"] = request.explicit_italic;
	object["explicit_charset"] = request.explicit_charset;
	object["explicit_height"] = request.explicit_height;
	object["baseline_name"] = request.baseline_name;
	object["baseline_weight"] = request.baseline_weight;
	object["baseline_italic"] = request.baseline_italic;
	object["baseline_charset"] = request.baseline_charset;
	object["baseline_height"] = request.baseline_height;
	object["styles"] = Array(request.styles);
	object["lines"] = Array(request.lines);
	object["override_lines"] = Array(request.override_lines);
	object["matched_name"] = request.matched_name;
	object["matched_full_name"] = request.matched_full_name;
	object["matched_weight"] = request.matched_weight;
	object["matched_bold"] = request.matched_bold;
	object["matched_italic"] = request.matched_italic;
	object["face_index"] = request.face_index;
	object["path_source"] = request.path_source;
	object["paths"] = Array(request.paths);
	object["memory_only"] = request.memory_only;
	return object;
}

void MergeDiagnostics(std::vector<Diagnostic>& diagnostics) {
	auto key = [](Diagnostic const& value) {
		return std::tie(value.input, value.code, value.severity, value.ass_name,
						value.requested_weight, value.requested_italic, value.message, value.codepoints);
	};
	std::ranges::sort(diagnostics, [&](auto const& a, auto const& b) {
		return key(a) < key(b);
	});
	std::vector<Diagnostic> merged;
	for (auto& diagnostic : diagnostics) {
		if (!merged.empty() && key(merged.back()) == key(diagnostic)) {
			auto& lines = merged.back().lines;
			lines.insert(lines.end(), diagnostic.lines.begin(), diagnostic.lines.end());
			SortUnique(lines);
		}
		else {
			SortUnique(diagnostic.lines);
			merged.push_back(std::move(diagnostic));
		}
	}
	diagnostics = std::move(merged);
}

}

std::string SystemDisplayLocale() {
#ifdef _WIN32
	ULONG count = 0;
	ULONG size = 0;
	if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
		std::wstring languages(size, L'\0');
		if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, languages.data(), &size) && count)
			return agi::charset::ConvertW(std::wstring(languages.c_str()));
	}
	wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
	if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0))
		return agi::charset::ConvertW(std::wstring(locale));
#else
	for (auto const *name : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
		auto const *value = std::getenv(name);
		if (!value || !*value) continue;
		std::string language(value);
		language.resize(language.find_first_of(":.@") == std::string::npos ? language.size() : language.find_first_of(":.@"));
		std::ranges::replace(language, '_', '-');
		return language;
	}
#endif
	return "und";
}

Result InvalidOptions(Options const& options, std::string message) {
	auto result = BaseResult(options);
	for (auto const& input : options.inputs) result.inputs.push_back({.path = agi::fs::PathToString(input), .kind = "unknown"});
	Fail(result, 1, "invalid_argument", {}, std::move(message));
	return result;
}

Result Scan(Options const& options) {
	if (options.low_frequency_threshold < 0) return InvalidOptions(options, "low frequency threshold must be nonnegative");
	if (options.inputs.empty()) return InvalidOptions(options, "at least one input is required");
	if (options.matcher != FontCollectorMatcher::Platform && options.matcher != FontCollectorMatcher::Libass)
		return InvalidOptions(options, "invalid matcher");
	if (options.matcher == FontCollectorMatcher::Platform &&
		(!options.provider_options.include_system_fonts || !options.provider_options.additional_font_files.empty()))
		return InvalidOptions(options, "private font options require the libass matcher");
	if (!options.provider_options.include_system_fonts && options.provider_options.additional_font_files.empty())
		return InvalidOptions(options, "a private font catalog requires additional font files");
	for (auto const& input : options.provider_options.additional_font_files) {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(agi::fs::PathFromString(input), ec) || ec)
			return InvalidOptions(options, "additional font input must be a readable font file: " + input);
	}
	if (!options.encoding.empty()) {
		try {
			agi::charset::IconvWrapper converter(options.encoding.c_str(), "utf-8");
		}
		catch (agi::Exception const& e) {
			return InvalidOptions(options, e.GetMessage());
		}
		catch (std::exception const& e) {
			return InvalidOptions(options, e.what());
		}
	}
	auto result = BaseResult(options);
	auto const files = ExpandInputs(options, result);
	if (files.empty()) return result;
	std::shared_ptr<FontFamilyCatalog const> catalog;
	try {
		result.catalog_generation = font_family_catalog_cache::GetGeneration();
		if (options.matcher == FontCollectorMatcher::Platform)
			catalog = font_family_catalog_cache::GetSnapshot();
	}
	catch (std::exception const& e) {
		Fail(result, 5, "font_cache_error", {}, e.what());
		return result;
	}

	struct Loaded {
		std::string input;
		size_t file_index = 0;
		std::unique_ptr<AssFile> subs;
		FontCollectorDetails details;
	};
	std::vector<Loaded> loaded;
	for (auto const& file : files) {
		auto const input = agi::fs::PathToString(file);
		result.files.push_back({.input = input, .status = "error"});
		try {
			Loaded item;
			item.input = input;
			item.file_index = result.files.size() - 1;
			item.subs = ReadFile(file, options.encoding);
			loaded.push_back(std::move(item));
		}
		catch (agi::Exception const& e) {
			Fail(result, 5, "file_error", input, e.GetMessage());
		}
		catch (std::exception const& e) {
			Fail(result, 5, "file_error", input, e.what());
		}
	}
	if (loaded.empty()) return result;
	try {
		auto event_sink = [&](FontCollectorEvent const& event) {
			if (event.type == FontCollectorEventType::FontCacheError)
				Fail(result, 5, "font_cache_error", {}, event.message);
		};
		auto provider_options = options.provider_options;
		provider_options.collect_match_candidates = true;
		FontCollectorSession session(event_sink, options.matcher, provider_options);
		IdentityResolver identities(options, result.locale, catalog);
		std::map<std::string, Row> rows;
		std::map<std::string, std::map<std::string, std::set<std::pair<int, int>>>> times;
		for (auto& item : loaded) {
			try {
				bool file_complete = true;
				session.GetFontPaths(item.subs.get(), [&](FontCollectorEvent const& event) {
					if (event.type == FontCollectorEventType::StyleMissing) {
						file_complete = false;
						Fail(result, 5, "missing_style", item.input, "style is referenced but not defined: " + event.style);
						result.diagnostics.back().lines = event.lines;
					}
					else if (event.type == FontCollectorEventType::FontCacheError) {
						file_complete = false;
						Fail(result, 5, "font_cache_error", item.input, event.message);
					} }, &item.details);
				result.files[item.file_index].status = file_complete ? "ok" : "error";
				for (auto const& original_usage : item.details.fonts) {
					std::optional<FontCollectorAssFontUsage> glyphless_usage;
					if (original_usage.matched.glyphless_family) {
						glyphless_usage = original_usage;
						glyphless_usage->matched = *original_usage.matched.glyphless_family;
					}
					auto const& usage = glyphless_usage ? *glyphless_usage : original_usage;
					if (usage.event_times.empty()) continue;
					bool const missing = usage.matched.paths.empty() && usage.matched.memory_fonts.empty();
					auto const identity = identities.Resolve(usage);
					auto const key = result.matcher + ":" + result.provider + ":" + identity.key;
					auto [found, inserted] = rows.try_emplace(key);
					auto& row = found->second;
					if (inserted) {
						row.key = key;
						row.selection_reliable = identity.reliable;
						row.localized_name = identity.localized;
						row.english_name = identity.english;
						row.display_name = row.english_name
											   ? row.localized_name.value_or(*row.english_name) + "<" + *row.english_name + ">"
											   : row.localized_name.value_or(identity.fallback);
						row.missing_font = missing;
						if (!missing) row.missing_codepoints.emplace();
					}
					row.ass_names.push_back(usage.ass_facename);
					row.requests.push_back(UsageRequest(item.input, usage));
					auto& event_times = times[key][item.input];
					event_times.insert(usage.event_times.begin(), usage.event_times.end());
					if (row.missing_codepoints) {
						row.missing_codepoints->insert(row.missing_codepoints->end(),
													   usage.matched.missing_codepoints.begin(), usage.matched.missing_codepoints.end());
					}
					auto add = [&](std::string code, std::string severity, std::string message) {
						auto diagnostic = UsageDiagnostic(std::move(code), std::move(severity), std::move(message), item.input, usage);
						if (diagnostic.severity == "warning") row.warnings.push_back(diagnostic);
						result.diagnostics.push_back(std::move(diagnostic));
					};
					if (missing) add("missing_font", "error", "font request could not be resolved");
					if (!usage.matched.missing_codepoints.empty() && !missing) {
						auto diagnostic = UsageDiagnostic("missing_glyphs", "error", "matched font is missing required glyphs", item.input, usage);
						diagnostic.codepoints = usage.matched.missing_codepoints;
						if (!usage.matched.missing_lines.empty()) diagnostic.lines = usage.matched.missing_lines;
						result.diagnostics.push_back(std::move(diagnostic));
					}
					if (usage.matched.match_candidates.size() > 1)
						add(usage.matched.match_ambiguous ? "ambiguous_font_match" : "font_match_candidates",
							usage.matched.match_ambiguous ? "warning" : "info",
							usage.matched.match_ambiguous ? "equal-score candidates can serve the same codepoint; provider order selected the face"
														  : "font was selected from multiple candidates; the libass score is a ranking, not proof of identity");
					if (usage.matched.fake_bold) add("fake_bold", "warning", "matched font needs synthetic bold");
					if (usage.matched.fake_italic) add("fake_italic", "warning", "matched font needs synthetic italic");
					if (usage.matched.implicit_variant_fallback) add("implicit_variant_fallback", "info", "font family resolved to a non-regular variant without an explicit ASS variant");
					if (usage.matched.noncanonical_variant) add("noncanonical_font_variant", "info", "realized font weight or style cannot be safely represented as Regular/Bold/Italic");
				}
			}
			catch (agi::Exception const& e) {
				Fail(result, 5, "file_error", item.input, e.GetMessage());
			}
			catch (std::exception const& e) {
				Fail(result, 5, "file_error", item.input, e.what());
			}
		}
		for (auto& [key, row] : rows) {
			SortUnique(row.ass_names);
			MergeDiagnostics(row.warnings);
			if (row.missing_codepoints) SortUnique(*row.missing_codepoints);
			for (auto const& [input, distinct] : times[key]) {
				row.sources.push_back({.input = input, .usage_count = distinct.size()});
				row.usage_count += distinct.size();
			}
			result.rows.push_back(std::move(row));
		}
		std::ranges::sort(result.rows, [](auto const& a, auto const& b) {
			auto const an = a.english_name.value_or(a.localized_name.value_or(a.ass_names.front()));
			auto const bn = b.english_name.value_or(b.localized_name.value_or(b.ass_names.front()));
			return std::tuple(Fold(an), an, a.key) < std::tuple(Fold(bn), bn, b.key);
		});
	}
	catch (agi::Exception const& e) {
		Fail(result, 5, "session_error", {}, e.GetMessage());
	}
	catch (std::exception const& e) {
		Fail(result, 5, "session_error", {}, e.what());
	}
	MergeDiagnostics(result.diagnostics);
	RecomputeThreshold(result, options.low_frequency_threshold);
	return result;
}

std::string FormatMissingCodepoints(std::span<uint32_t const> values) {
	std::string result;
	for (auto value : values) {
		if (!result.empty()) result += ", ";
		if (value > 0x20 && value != 0x7F && !(value >= 0x80 && value <= 0x9F) &&
			value != 0xA0 && value != 0xAD && !(value >= 0x2000 && value <= 0x200F) &&
			!(value >= 0x2028 && value <= 0x202F) && !(value >= 0x2060 && value <= 0x206F) &&
			value != 0xFEFF) {
			font_collector::unicode::Rune rune;
			if (font_collector::unicode::Rune::TryCreate(value, rune))
				font_collector::unicode::AppendRuneToUtf8(result, rune);
			result += " ";
		}
		std::ostringstream codepoint;
		codepoint << "U+" << std::uppercase << std::hex << std::setfill('0') << std::setw(4) << value;
		result += codepoint.str();
	}
	return result;
}

void RecomputeThreshold(Result& result, int threshold) {
	if (threshold < 0) throw std::invalid_argument("low frequency threshold must be nonnegative");
	result.low_frequency_threshold = threshold;
	for (auto& row : result.rows) {
		row.is_low_usage = threshold > 0 && std::cmp_less(row.usage_count, threshold);
		row.reasons.clear();
		if (row.missing_font) row.reasons.push_back({.code = "missing_font", .message = "Not found"});
		if (row.missing_codepoints && !row.missing_codepoints->empty())
			row.reasons.push_back({.code = "missing_glyphs", .message = "Missing glyphs: " + FormatMissingCodepoints(*row.missing_codepoints)});
		std::set<std::string> codes;
		for (auto const& warning : row.warnings)
			if (codes.insert(warning.code).second)
				row.reasons.push_back({.code = warning.code, .message = WarningMessage(warning.code)});
		if (row.is_low_usage) row.reasons.push_back({.code = "low_usage", .message = "Low usage"});
		row.display_state = row.missing_font                                             ? "missing_font"
							: row.missing_codepoints && !row.missing_codepoints->empty() ? "missing_glyphs"
							: !row.warnings.empty()                                      ? "warning"
							: row.is_low_usage                                           ? "low_usage"
																						 : "normal";
	}
}

json::Object ToJson(Result const& result) {
	json::Object object;
	object["format"] = "font_usage_inventory";
	object["schema_version"] = static_cast<json::Integer>(1);
	object["locale"] = result.locale;
	object["matcher"] = result.matcher;
	object["provider"] = result.provider;
	object["recursive"] = result.recursive;
	object["low_frequency_threshold"] = static_cast<json::Integer>(result.low_frequency_threshold);
	object["complete"] = result.complete;
	object["result"] = static_cast<json::Integer>(result.result);
	json::Array inputs;
	for (auto const& input : result.inputs) {
		json::Object value;
		value["path"] = input.path;
		value["kind"] = input.kind;
		inputs.emplace_back(std::move(value));
	}
	object["inputs"] = std::move(inputs);
	json::Array files;
	for (auto const& file : result.files) {
		json::Object value;
		value["input"] = file.input;
		value["status"] = file.status;
		files.emplace_back(std::move(value));
	}
	object["files"] = std::move(files);
	json::Array fonts;
	for (auto const& row : result.rows) {
		json::Object font;
		font["ass_names"] = Array(row.ass_names);
		font["localized_name"] = row.localized_name ? json::UnknownElement(*row.localized_name) : json::UnknownElement(json::Null());
		font["english_name"] = row.english_name ? json::UnknownElement(*row.english_name) : json::UnknownElement(json::Null());
		font["display_name"] = row.display_name;
		font["usage_count"] = static_cast<json::Integer>(row.usage_count);
		font["is_low_usage"] = row.is_low_usage;
		font["missing_font"] = row.missing_font;
		font["missing_codepoints"] = row.missing_codepoints ? json::UnknownElement(Codepoints(*row.missing_codepoints)) : json::UnknownElement(json::Null());
		font["warnings"] = DiagnosticsJson(row.warnings);
		font["display_state"] = row.display_state;
		json::Array reasons;
		for (auto const& reason : row.reasons) {
			json::Object value;
			value["code"] = reason.code;
			value["message"] = reason.message;
			reasons.emplace_back(std::move(value));
		}
		font["reasons"] = std::move(reasons);
		json::Array sources;
		for (auto const& source : row.sources) {
			json::Object value;
			value["input"] = source.input;
			value["usage_count"] = static_cast<json::Integer>(source.usage_count);
			sources.emplace_back(std::move(value));
		}
		font["sources"] = std::move(sources);
		json::Array requests;
		for (auto const& request : row.requests) requests.emplace_back(RequestJson(request));
		font["requests"] = std::move(requests);
		fonts.emplace_back(std::move(font));
	}
	object["fonts"] = std::move(fonts);
	object["diagnostics"] = DiagnosticsJson(result.diagnostics);
	return object;
}

}
