// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "font_file_lister.h"

#include "font_collector_unicode.h"
#include "font_matching_common.h"

#include <libaegisub/fs.h>
#include <libaegisub/string_utils.h>

#include <fontconfig/fontconfig.h>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void Emit(FontCollectorEventSink const& sink, FontCollectorEvent event) {
	if (sink)
		sink(event);
}

#ifdef _WIN32
std::string WideToUtf8(std::wstring const& value) {
	if (value.empty())
		return {};

	auto len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (len <= 0)
		return {};

	std::string text(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), text.data(), len, nullptr, nullptr);
	return text;
}

bool DirectoryExists(std::wstring const& path) {
	auto attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFontFile(std::filesystem::path const& path) {
	auto ext = path.extension().wstring();
	for (auto& ch : ext)
		ch = static_cast<wchar_t>(std::towlower(ch));
	return ext == L".ttf" || ext == L".ttc" || ext == L".otf" || ext == L".otc" || ext == L".woff" || ext == L".woff2";
}

std::vector<std::wstring> GetWindowsFontDirs() {
	std::vector<std::wstring> dirs;

	wchar_t windows_dir[MAX_PATH] = {};
	if (GetWindowsDirectoryW(windows_dir, MAX_PATH)) {
		std::wstring font_dir = windows_dir;
		font_dir += L"\\Fonts";
		if (DirectoryExists(font_dir))
			dirs.push_back(font_dir);
	}

	auto local_app_data_len = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
	if (local_app_data_len > 1) {
		std::wstring local_app_data(local_app_data_len, L'\0');
		if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), local_app_data_len)) {
			while (!local_app_data.empty() && local_app_data.back() == L'\0')
				local_app_data.pop_back();

			std::wstring user_font_dir = local_app_data + L"\\Microsoft\\Windows\\Fonts";
			if (DirectoryExists(user_font_dir))
				dirs.push_back(user_font_dir);
		}
	}

	return dirs;
}

void AddWindowsFontFiles(FcConfig *config) {
	for (auto const& dir : GetWindowsFontDirs()) {
		std::error_code ec;
		auto const font_dir = std::filesystem::path{dir};
		std::filesystem::recursive_directory_iterator it(
			font_dir,
			std::filesystem::directory_options::skip_permission_denied,
			ec);
		std::filesystem::recursive_directory_iterator end;
		for (; !ec && it != end; it.increment(ec)) {
			std::error_code status_ec;
			if (!it->is_regular_file(status_ec) || status_ec || !IsFontFile(it->path()))
				continue;

			auto path = WideToUtf8(it->path().wstring());
			if (!path.empty())
				FcConfigAppFontAddFile(config, reinterpret_cast<FcChar8 const *>(path.c_str()));
		}
	}
}

FcConfig *CreateFontConfig() {
	if (auto config = FcInitLoadConfig())
		return config;

	auto config = FcConfigCreate();
	if (!config)
		return nullptr;

	AddWindowsFontFiles(config);

	return config;
}
#else
FcConfig *CreateFontConfig() {
	return FcInitLoadConfig();
}
#endif

void AddAdditionalFontFiles(FcConfig *config, FontProviderOptions const& options, FontCollectorEventSink const& sink) {
	if (!config) {
		if (!options.additional_font_files.empty() && sink) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::FontCacheError;
			event.message = "failed to create the fontconfig configuration";
			sink(event);
		}
		return;
	}
	for (auto const& path : options.additional_font_files)
		if (FcConfigAppFontAddFile(config, reinterpret_cast<FcChar8 const *>(path.c_str())) == FcFalse && sink) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::FontCacheError;
			event.message = "failed to load an additional font file in fontconfig: " + path;
			sink(event);
		}
}

bool pattern_matches(FcPattern *pat, const char *field, std::string const& name) {
	FcChar8 *str;
	for (int i = 0; FcPatternGetString(pat, field, i, &str) == FcResultMatch; ++i) {
		std::string sstr((char *)str);
		agi::util::strings::to_lower_inplace(sstr);
		if (sstr == name)
			return true;
	}
	return false;
}

void find_font(FcFontSet *src, FcFontSet *dst, std::string const& family) {
	if (!src) return;

	for (int i = 0; i < src->nfont; ++i) {
		FcPattern *pat = src->fonts[i];
		int val;
		if (FcPatternGetBool(pat, FC_OUTLINE, 0, &val) != FcResultMatch || val != FcTrue) continue;

		if (pattern_matches(pat, FC_FULLNAME, family) || pattern_matches(pat, FC_FAMILY, family))
			FcFontSetAdd(dst, FcPatternDuplicate(pat));
	}
}

int FontconfigWeightFromOpenType(int weight) {
#if FC_VERSION >= 21292
	return static_cast<int>(std::lround(FcWeightFromOpenTypeDouble(weight)));
#else
	return FcWeightFromOpenType(weight);
#endif
}

int OpenTypeWeightFromFontconfig(FcPattern *pattern) {
	double fc_weight = 0;
	if (FcPatternGetDouble(pattern, FC_WEIGHT, 0, &fc_weight) == FcResultMatch) {
#if FC_VERSION >= 21292
		return static_cast<int>(std::lround(FcWeightToOpenTypeDouble(fc_weight)));
#else
		return FcWeightToOpenType(static_cast<int>(std::lround(fc_weight)));
#endif
	}

	int fc_weight_int = 0;
	if (FcPatternGetInteger(pattern, FC_WEIGHT, 0, &fc_weight_int) == FcResultMatch) {
#if FC_VERSION >= 21292
		return static_cast<int>(std::lround(FcWeightToOpenTypeDouble(fc_weight_int)));
#else
		return FcWeightToOpenType(fc_weight_int);
#endif
	}

	return 400;
}

}

FontConfigFontFileLister::FontConfigFontFileLister(
	FontCollectorEventSink &cb,
	bool build_libass_catalog,
	FontProviderOptions const& options)
: config(options.include_system_fonts ? CreateFontConfig() : FcConfigCreate(), FcConfigDestroy)
{
	FontCollectorEvent event;
	if (!build_libass_catalog) {
		event.type = FontCollectorEventType::FontBackendInfo;
		event.message = "fontconfig";
		Emit(cb, std::move(event));
	}

	event = FontCollectorEvent();
	event.type = FontCollectorEventType::UpdatingFontCache;
	Emit(cb, std::move(event));
	AddAdditionalFontFiles(config, options, cb);
	if (config && FcConfigBuildFonts(config) == FcFalse) {
		event.type = FontCollectorEventType::FontCacheError;
		event.message = "fontconfig failed to build the font catalog";
		Emit(cb, std::move(event));
	}
	if (build_libass_catalog)
		BuildLibassCatalog();
}

FontConfigFontFileLister::~FontConfigFontFileLister() {
	if (libass_fallback_chars)
		FcCharSetDestroy(static_cast<FcCharSet *>(libass_fallback_chars));
	if (libass_fallbacks)
		FcFontSetDestroy(libass_fallbacks);
	for (auto *pattern : libass_patterns)
		FcPatternDestroy(static_cast<FcPattern *>(pattern));
}

void FontConfigFontFileLister::BuildLibassCatalog() {
	if (!config)
		return;

	agi::scoped_holder<FcPattern *> sort_pattern(FcPatternCreate(), FcPatternDestroy);
	if (!sort_pattern)
		return;
#if FC_VERSION >= 21700
	FcConfigSetDefaultSubstitute(config, sort_pattern);
#else
	FcDefaultSubstitute(sort_pattern);
#endif

	FcResult sort_result = FcResultNoMatch;
	agi::scoped_holder<FcFontSet *> fonts(
		FcFontSort(config, sort_pattern, FcFalse, nullptr, &sort_result), FcFontSetDestroy);
	if (!fonts || sort_result != FcResultMatch)
		return;

	for (int i = 0; i < fonts->nfont; ++i) {
		auto *pattern = FcPatternDuplicate(fonts->fonts[i]);
		if (!pattern)
			continue;

		FcBool outline = FcFalse;
		FcChar8 *file = nullptr;
		int face_index = 0;
		int slant = FC_SLANT_ROMAN;
		if (FcPatternGetBool(pattern, FC_OUTLINE, 0, &outline) != FcResultMatch || outline != FcTrue ||
		    FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch ||
		    FcPatternGetInteger(pattern, FC_INDEX, 0, &face_index) != FcResultMatch ||
		    FcPatternGetInteger(pattern, FC_SLANT, 0, &slant) != FcResultMatch) {
			FcPatternDestroy(pattern);
			continue;
		}

		LibassFontFace face;
		for (int n = 0;; ++n) {
			FcChar8 *value = nullptr;
			if (FcPatternGetString(pattern, FC_FAMILY, n, &value) != FcResultMatch)
				break;
			face.families.emplace_back(reinterpret_cast<char const *>(value));
		}
		for (int n = 0;; ++n) {
			FcChar8 *value = nullptr;
			if (FcPatternGetString(pattern, FC_FULLNAME, n, &value) != FcResultMatch)
				break;
			face.fullnames.emplace_back(reinterpret_cast<char const *>(value));
		}
		FcChar8 *value = nullptr;
		if (FcPatternGetString(pattern, FC_POSTSCRIPT_NAME, 0, &value) == FcResultMatch)
			face.postscript_name = reinterpret_cast<char const *>(value);
		face.path = reinterpret_cast<char const *>(file);
		face.face_index = face_index;
		face.weight = OpenTypeWeightFromFontconfig(pattern);
		// libass's fontconfig provider only records italic in style_flags;
		// bold is intentionally inferred later from the OpenType weight.
		face.bold = false;
		face.italic = slant >= FC_SLANT_ITALIC;
		FcChar8 *format = nullptr;
		if (FcPatternGetString(pattern, FC_FONTFORMAT, 0, &format) == FcResultMatch) {
			std::string format_name(reinterpret_cast<char const *>(format));
			face.postscript_outlines = format_name == "CFF" || format_name == "Type 1" ||
			                           format_name == "Type 42" || format_name == "CID Type 1";
		}

		FcCharSet *charset = nullptr;
		FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset);
		libass_faces.push_back(std::move(face));
		libass_patterns.push_back(pattern);
		libass_charsets.push_back(charset);
	}
}

std::vector<std::string> FontConfigFontFileLister::GetLibassSubstitutions(std::string_view family) const {
	std::vector<std::string> result;
	if (!config)
		return result;

	constexpr char delimiter[] = "__libass_delimiter";
	agi::scoped_holder<FcPattern *> pattern(FcPatternCreate(), FcPatternDestroy);
	if (!pattern)
		return result;

	auto name = std::string(family);
	FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<FcChar8 const *>(name.c_str()));
	FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<FcChar8 const *>(delimiter));
	FcPatternAddBool(pattern, FC_OUTLINE, FcTrue);
	if (!FcConfigSubstitute(config, pattern, FcMatchPattern))
		return result;

	for (int i = 0; i < 100; ++i) {
		FcChar8 *alias = nullptr;
		if (FcPatternGetString(pattern, FC_FAMILY, i, &alias) != FcResultMatch)
			break;
		auto value = reinterpret_cast<char const *>(alias);
		if (std::string_view(value) == delimiter)
			break;
		result.emplace_back(value);
	}
	return result;
}

std::optional<std::string> FontConfigFontFileLister::GetLibassFallback(std::string_view, uint32_t codepoint) {
	if (!config)
		return std::nullopt;

	if (!libass_fallbacks) {
		agi::scoped_holder<FcPattern *> pattern(FcPatternCreate(), FcPatternDestroy);
		if (!pattern)
			return std::nullopt;
		FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<FcChar8 const *>("sans-serif"));
		FcPatternAddBool(pattern, FC_OUTLINE, FcTrue);
		FcConfigSubstitute(config, pattern, FcMatchPattern);
#if FC_VERSION >= 21700
		FcConfigSetDefaultSubstitute(config, pattern);
#else
		FcDefaultSubstitute(pattern);
#endif
		FcPatternDel(pattern, FC_LANG);

		FcResult match_result = FcResultNoMatch;
		FcCharSet *fallback_chars = nullptr;
		libass_fallbacks = FcFontSort(config, pattern, FcTrue, &fallback_chars, &match_result);
		libass_fallback_chars = fallback_chars;
		if (match_result != FcResultMatch) {
			if (libass_fallbacks)
				FcFontSetDestroy(libass_fallbacks);
			libass_fallbacks = FcFontSetCreate();
		}
	}

	if (!libass_fallbacks || libass_fallbacks->nfont == 0)
		return std::nullopt;
	if (codepoint && (!libass_fallback_chars ||
	    FcCharSetHasChar(static_cast<FcCharSet *>(libass_fallback_chars), codepoint) == FcFalse))
		return std::nullopt;

	for (int i = 0; i < libass_fallbacks->nfont; ++i) {
		auto *pattern = libass_fallbacks->fonts[i];
		if (codepoint) {
			FcCharSet *charset = nullptr;
			if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) != FcResultMatch ||
			    FcCharSetHasChar(charset, codepoint) == FcFalse)
				continue;
		}

		FcChar8 *family = nullptr;
		if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch)
			return std::string(reinterpret_cast<char const *>(family));
		return std::nullopt;
	}
	return std::nullopt;
}

bool FontConfigFontFileLister::HasLibassGlyph(size_t face_index, uint32_t codepoint) const {
	if (codepoint == 0)
		return true;
	if (face_index >= libass_charsets.size() || !libass_charsets[face_index])
		return false;
	return FcCharSetHasChar(static_cast<FcCharSet *>(libass_charsets[face_index]), codepoint) == FcTrue;
}

CollectionResult FontConfigFontFileLister::GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters) {
	CollectionResult ret;
	if (!config)
		return ret;

	std::string family = facename[0] == '@' ? facename.substr(1) : facename;
	agi::util::strings::to_lower_inplace(family);

	int requested_weight = NormalizeLibassAssWeight(bold);
	int weight = FontconfigWeightFromOpenType(requested_weight);
	int slant  = italic ? 110 : 0;
	ret.requested_weight = requested_weight;

	// Create a fontconfig pattern to match the desired weight/slant
	agi::scoped_holder<FcPattern*> pat(FcPatternCreate(), FcPatternDestroy);
	if (!pat) return ret;

	FcPatternAddBool(pat, FC_OUTLINE, true);
	FcPatternAddInteger(pat, FC_SLANT, slant);
	FcPatternAddInteger(pat, FC_WEIGHT, weight);

	#if FC_VERSION >= 21700
	FcConfigSetDefaultSubstitute(config, pat);
#else
	FcDefaultSubstitute(pat);
#endif
	if (!FcConfigSubstitute(config, pat, FcMatchPattern)) return ret;

	// Create a font set with only correctly named fonts
	// This is needed because the patterns returned by font matching only
	// include the first family and fullname, so we can't always verify that
	// we got the actual font we were asking for after the fact
	agi::scoped_holder<FcFontSet*> fset(FcFontSetCreate(), FcFontSetDestroy);
	find_font(FcConfigGetFonts(config, FcSetApplication), fset, family);
	find_font(FcConfigGetFonts(config, FcSetSystem), fset, family);

	// Get the best match from fontconfig
	FcResult result;
	FcFontSet *sets[] = { (FcFontSet*)fset };

	agi::scoped_holder<FcFontSet*> matches(FcFontSetSort(config, sets, 1, pat, false, nullptr, &result), FcFontSetDestroy);
	if (matches->nfont == 0)
		return ret;

	auto match = matches->fonts[0];

	FcChar8 *matched_family;
	if (FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch)
		ret.matched_facename = reinterpret_cast<char const *>(matched_family);
	for (int i = 0; FcPatternGetString(match, FC_FAMILY, i, &matched_family) == FcResultMatch; ++i)
		ret.matched_names.emplace_back(reinterpret_cast<char const *>(matched_family));
	FcPatternGetInteger(match, FC_INDEX, 0, &ret.face_index);
	ret.matched_weight = OpenTypeWeightFromFontconfig(match);
	ret.matched_bold = ret.matched_weight > 550;
	int matched_slant = 0;
	if (FcPatternGetInteger(match, FC_SLANT, 0, &matched_slant) == FcResultMatch)
		ret.matched_italic = matched_slant != FC_SLANT_ROMAN;

	FcChar8 *file;
	if(FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
		return ret;

	FcCharSet *charset;
	if (FcPatternGetCharSet(match, FC_CHARSET, 0, &charset) == FcResultMatch) {
		for (auto chr : characters) {
			font_collector::unicode::Rune rune;
			if (!font_collector::unicode::Rune::TryCreate(chr, rune))
				continue;
			if (!FcCharSetHasChar(charset, rune.Value()))
				font_collector::unicode::AppendRuneToUtf8(ret.missing, rune);
		}
	}

	ret.fake_bold = requested_weight > ret.matched_weight + 150 && !ret.matched_bold;

	int actual_slant = slant;
	if (FcPatternGetInteger(match, FC_SLANT, 0, &actual_slant) == FcResultMatch)
		ret.fake_italic = italic && !actual_slant;

	ret.paths.emplace_back((const char *)file);
	ret.path_source = "fontconfig";
	return ret;
}

CollectionResult FontConfigFontFileLister::GetFontPaths(
	aegisub::ass::AssFontRequest const& request,
	std::vector<uint32_t> const& characters) {
	auto result = GetFontPaths(
		request.family,
		aegisub::ass::LegacyAssBoldArgument(request),
		request.italic,
		characters);
	if (!result.backend_requested_weight)
		result.backend_requested_weight = result.requested_weight;
	return result;
}

FontFileListerMatchKey FontConfigFontFileLister::GetMatchKey(
	aegisub::ass::AssFontRequest const& request) const {
	auto normalized = NormalizeLibassFontRequest(
		request.family,
		aegisub::ass::LegacyAssBoldArgument(request),
		request.italic);
	return {
		std::move(normalized.facename),
		normalized.requested_weight,
		normalized.requested_italic,
	};
}
