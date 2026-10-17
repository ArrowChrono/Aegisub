// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
// Copyright (c) 2026, MIRIMIRIM
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

#include "font_collector_backend.h"
#include "font_collector_events.h"
#include "ass_font_state.h"
#include "font_family_catalog.h"

// Libass provider surface lives in font_matching_libass.h. Pull it only for
// Fontconfig-backed builds (Linux always; Apple when Fontconfig is enabled).
// Windows uses a forward declaration and includes the full header in .cpp files.
#if !defined(_WIN32) && !defined(__APPLE__) || defined(AEGISUB_FONTCOLLECTOR_ENABLE_FONTCONFIG)
#include "font_matching_libass.h"
#else
class ILibassFontProvider;
#endif

#include <libaegisub/fs_fwd.h>
#include <libaegisub/scoped_ptr.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class AssDialogue;
class AssFile;
class AssStyle;

struct FontCollectorMatchCandidate {
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

struct FontMemoryFont {
	std::string facename;
	std::shared_ptr<std::vector<char> const> data;
};

struct CollectionResult {
	/// Font face selected by the platform matcher.
	std::string matched_facename;
	/// Full font face selected by the platform matcher, when available.
	std::string matched_facename_full;
	/// Matched family aliases reported by the platform provider.
	std::vector<std::string> matched_names;
	agi::fs::path primary_path;
	std::shared_ptr<std::vector<char> const> primary_memory_data;
	int face_index = -1;
	/// Font weight selected by the platform matcher.
	int matched_weight = 0;
	/// Whether the resolver proved a canonical Bold/BoldItalic role. Numeric
	/// weights such as 600/800/900 and unknown entities remain false here.
	bool matched_bold = false;
	/// Whether the selected platform font is italic.
	bool matched_italic = false;
	/// Characters which could not be found in any font files
	std::string missing;
	/// Paths to the file(s) containing the requested font
	std::vector<agi::fs::path> paths;
	/// Selected fonts whose bytes are available but whose file path is not.
	std::vector<FontMemoryFont> memory_fonts;
	/// How the font file path was resolved: "dwritecore", "dwrite", "gdi", or empty.
	std::string path_source;
	bool fake_bold = false;
	bool fake_italic = false;
	/// The lfWeight value passed to CreateFontIndirectW.
	int requested_weight = 0;
	/// The backend request after ASS normalization, when known. This is
	/// optional to keep old/custom listers source-compatible.
	std::optional<int> backend_requested_weight;
	/// Canonical role/status of the realized face, when the backend can prove it.
	std::optional<FontVariantRole> realized_role;
	std::optional<FontVariantStatus> realized_status;
	/// The backend selected a non-regular face without an explicit ASS variant.
	bool implicit_variant_fallback = false;
	/// The realized face is not safe to canonicalize as Regular/Bold/Italic.
	bool noncanonical_variant = false;
	std::vector<FontCollectorMatchCandidate> match_candidates;
	bool match_ambiguous = false;
	std::shared_ptr<CollectionResult const> glyphless_family;
};

struct FontCollectorMatchedFont {
	std::string facename;
	std::string facename_full;
	std::vector<std::string> names;
	agi::fs::path primary_path;
	std::shared_ptr<std::vector<char> const> primary_memory_data;
	int face_index = -1;
	int weight = 0;
	bool bold = false;
	bool italic = false;
	std::vector<agi::fs::path> paths;
	std::vector<FontMemoryFont> memory_fonts;
	std::string path_source;
	bool fake_bold = false;
	bool fake_italic = false;
	std::string missing_text;
	std::vector<uint32_t> missing_codepoints;
	std::vector<int> missing_lines;
	int requested_weight = 0;
	std::optional<int> backend_requested_weight;
	std::optional<FontVariantRole> realized_role;
	std::optional<FontVariantStatus> realized_status;
	bool implicit_variant_fallback = false;
	bool noncanonical_variant = false;
	std::vector<FontCollectorMatchCandidate> match_candidates;
	bool match_ambiguous = false;
	std::shared_ptr<FontCollectorMatchedFont const> glyphless_family;
};

struct FontCollectorAssFontUsage {
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
	std::vector<std::string> styles;
	std::vector<int> lines;
	std::vector<int> override_lines;
	std::vector<std::pair<int, int>> event_times;
	FontCollectorMatchedFont matched;
};

struct FontCollectorDetails {
	std::vector<FontCollectorAssFontUsage> fonts;
};

struct FontCollectorBatchSource {
	AssFile const *file = nullptr;
	FontCollectorEventSink event_sink;
	FontCollectorDetails *details = nullptr;
};

/// Backend-normalized identity of one physical font match request.
///
/// Raw ASS provenance deliberately does not live here. The collector keeps it
/// in StyleInfo for reporting, while requests which a backend treats
/// identically share one potentially expensive font lookup.
struct FontFileListerMatchKey {
	std::string family;
	int weight = aegisub::ass::DefaultFontWeight;
	bool italic = false;
	std::optional<int> charset;
	/// Backend-native height request. Windows GDI stores the quantized
	/// LOGFONT.lfHeight here; outline backends leave it empty when size does
	/// not participate in physical face matching.
	std::optional<int> height;

	bool operator<(FontFileListerMatchKey const& other) const {
		return std::tie(family, weight, italic, charset, height) <
		       std::tie(other.family, other.weight, other.italic, other.charset,
		                 other.height);
	}
};

class IFontFileLister {
public:
	virtual ~IFontFileLister() = default;
	virtual CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters) = 0;

	/// Return the fields which determine the backend's physical selection.
	/// The default mirrors the rich-to-legacy adapter below. A lister which
	/// overrides the rich entry point and consumes additional fields must also
	/// override this method.
	virtual FontFileListerMatchKey GetMatchKey(
		aegisub::ass::AssFontRequest const& request) const {
		return {
			request.family,
			aegisub::ass::LegacyAssBoldFromEffectiveWeight(request.effective_weight),
			request.italic,
		};
	}

	/// Rich request entry point. Existing/custom listers only need to
	/// implement the legacy overload; this adapter preserves source
	/// compatibility while allowing the collector to carry charset, size and
	/// raw ASS provenance.
	virtual CollectionResult GetFontPaths(aegisub::ass::AssFontRequest const& request, std::vector<uint32_t> const& characters) {
		int const legacy_bold = aegisub::ass::LegacyAssBoldFromEffectiveWeight(
			request.effective_weight);
		auto result = GetFontPaths(request.family, legacy_bold, request.italic, characters);
		if (!result.backend_requested_weight)
			result.backend_requested_weight = request.effective_weight;
		return result;
	}
};

class LibassFontFileLister final : public IFontFileLister {
	std::unique_ptr<ILibassFontProvider> provider;
	bool collect_match_candidates = false;

public:
	using IFontFileLister::GetFontPaths;

	LibassFontFileLister(FontCollectorEventSink& event_sink,
	                     std::unique_ptr<ILibassFontProvider> provider,
	                     bool collect_match_candidates = false);
	~LibassFontFileLister() override;

	CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic,
	                              std::vector<uint32_t> const& characters) override;
	CollectionResult GetFontPaths(aegisub::ass::AssFontRequest const& request,
	                              std::vector<uint32_t> const& characters) override;
	FontFileListerMatchKey GetMatchKey(
		aegisub::ass::AssFontRequest const& request) const override;
};

class GdiFontResolver;

#ifdef _WIN32
std::unique_ptr<ILibassFontProvider> CreateDWriteLibassFontProvider(
	FontCollectorEventSink& event_sink,
	FontProviderOptions const& options = {});

class GdiFontFileLister : public IFontFileLister {
	std::unique_ptr<GdiFontResolver> resolver;
	std::unordered_multimap<uint32_t, agi::fs::path> index;
	std::string buffer;
	CollectionResult GetFontPathsImpl(
		std::string const& facename,
		int bold,
		bool italic,
		int charset,
		int height,
		std::vector<uint32_t> const& characters);

public:
	using IFontFileLister::GetFontPaths;

	/// Constructor
	/// @param cb Callback for status logging
	GdiFontFileLister(FontCollectorEventSink &cb);
	~GdiFontFileLister();

	/// @brief Get the path to the font with the given styles
	/// @param facename Name of font face
	/// @param bold ASS font weight
	/// @param italic Italic?
	/// @param characters Characters in this style
	/// @return Path to the matching font file(s), or empty if not found
	CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters) override;
	CollectionResult GetFontPaths(aegisub::ass::AssFontRequest const& request,
	                              std::vector<uint32_t> const& characters) override;
	FontFileListerMatchKey GetMatchKey(
		aegisub::ass::AssFontRequest const& request) const override;
};

#elif defined(__APPLE__)

struct CoreTextFontFileLister : public IFontFileLister {
	using IFontFileLister::GetFontPaths;

	CoreTextFontFileLister(FontCollectorEventSink &cb);

	/// @brief Get the path to the font with the given styles
	/// @param facename Name of font face
	/// @param bold ASS font weight
	/// @param italic Italic?
	/// @param characters Characters in this style
	/// @return Path to the matching font file(s), or empty if not found
	CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters) override;
};

#endif

#if !defined(_WIN32) && !defined(__APPLE__) || defined(AEGISUB_FONTCOLLECTOR_ENABLE_FONTCONFIG)

typedef struct _FcConfig FcConfig;
typedef struct _FcFontSet FcFontSet;

/// @class FontConfigFontFileLister
/// @brief fontconfig powered font lister
class FontConfigFontFileLister : public IFontFileLister, public ILibassFontProvider {
	agi::scoped_holder<FcConfig*> config;
	std::vector<LibassFontFace> libass_faces;
	std::vector<void *> libass_patterns;
	std::vector<void *> libass_charsets;
	FcFontSet *libass_fallbacks = nullptr;
	void *libass_fallback_chars = nullptr;

	void BuildLibassCatalog();

	/// @brief Case-insensitive match ASS/SSA font family against full name. (also known as "name for humans")
	/// @param family font fullname
	/// @param bold weight attribute
	/// @param italic italic attribute
	/// @return font set
	FcFontSet *MatchFullname(const char *family, int weight, int slant);
public:
	using IFontFileLister::GetFontPaths;

	/// Constructor
	/// @param cb Callback for status logging
	FontConfigFontFileLister(FontCollectorEventSink &cb,
	                         bool build_libass_catalog = false,
	                         FontProviderOptions const& options = {});
	~FontConfigFontFileLister();

	std::span<LibassFontFace const> GetLibassFaces() const override { return libass_faces; }
	bool HasLibassGlyph(size_t face_index, uint32_t codepoint) const override;
	std::vector<std::string> GetLibassSubstitutions(std::string_view family) const override;
	std::optional<std::string> GetLibassFallback(std::string_view family, uint32_t codepoint) override;
	std::string_view GetLibassProviderName() const override { return "fontconfig"; }

	/// @brief Get the path to the font with the given styles
	/// @param facename Name of font face
	/// @param bold ASS font weight
	/// @param italic Italic?
	/// @param characters Characters in this style
	/// @return Path to the matching font file(s), or empty if not found
	CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters) override;
	CollectionResult GetFontPaths(aegisub::ass::AssFontRequest const& request,
	                              std::vector<uint32_t> const& characters) override;
	FontFileListerMatchKey GetMatchKey(
		aegisub::ass::AssFontRequest const& request) const override;
};

#endif

#if defined(__APPLE__)
using FontFileLister = CoreTextFontFileLister;
#elif defined(_WIN32)
using FontFileLister = GdiFontFileLister;
#else
using FontFileLister = FontConfigFontFileLister;
#endif

/// @class FontCollector
/// @brief Class which collects the paths to all fonts used in a script
class FontCollector {
	/// All data needed to find the font file used to render text
	struct StyleInfo {
		aegisub::ass::AssFontRequest request;
		aegisub::ass::AssFontStyleBaseline event_style;
		bool operator<(StyleInfo const& rgt) const;
	};

	/// Codepoints and source locations used by one resolved ASS font request
	struct UsageData {
		std::array<uint64_t, 4> latin1_codepoints{}; ///< Packed U+0000..U+00FF set for ASCII-heavy text
		std::vector<uint32_t> codepoints;             ///< Sorted unique codepoints passed to the font lister
		std::vector<uint32_t> pending_codepoints;     ///< Batched U+0100+ codepoints waiting to be merged
		std::vector<int> lines;                       ///< Dialogue lines which use this font request
		std::vector<int> override_lines;              ///< Lines on which overrides select this font request
		std::vector<std::string> styles;              ///< ASS styles which resolve to this font request
		std::vector<std::pair<int, int>> event_times;
	};

	using MissingStyleLines = std::map<std::string, std::vector<int>>;

	struct FileAnalysis {
		AssFile const *file = nullptr;
		int wrap_style = 0;
		std::map<StyleInfo, UsageData> used_styles;
		MissingStyleLines missing_style_lines;
		std::vector<agi::fs::path> results;
		FontCollectorEventSink event_sink;
		FontCollectorDetails *details = nullptr;
		int missing = 0;
		int missing_glyphs = 0;
	};

	struct ResolvedUsage {
		CollectionResult result;
		std::vector<uint32_t> missing_codepoints;
	};

	/// Deferred line lookup for glyphs reported missing by the platform font lister
	struct MissingGlyphQuery {
		StyleInfo style;
		std::vector<uint32_t> missing_codepoints;
		std::vector<int> matching_lines;
		FontCollectorEvent event;
		UsageData const *usage = nullptr;
		std::shared_ptr<FontCollectorMatchedFont> internal_target;
	};

	/// Message callback provider by caller
	FontCollectorEventSink event_sink;

	std::unique_ptr<IFontFileLister> owned_lister;
	IFontFileLister *lister = nullptr;

	/// Walk plain text spans with their active resolved font request
	StyleInfo MakeStyleInfo(AssStyle const& style) const;
	void RecordMissingStyle(MissingStyleLines& missing_style_lines, std::string const& name, int line_index);
	void EmitMissingStyles(FileAnalysis& analysis);
	template<class Callback>
	bool ForEachLineTextSpan(AssFile const& file, AssDialogue const& line, int line_index, int wrap_style,
		Callback&& callback, MissingStyleLines *missing_style_lines);
	void ProcessDialogueLine(FileAnalysis& analysis, const AssDialogue *line, int index);
	void AddCodepoint(UsageData& data, uint32_t codepoint);
	void AppendTextCodepoints(std::string_view text, int wrap_style, UsageData& data);
	void MergePendingCodepoints(UsageData& data);
	void FinalizeUsageCodepoints(UsageData& data);
	FileAnalysis AnalyzeFile(FontCollectorBatchSource const& source);

	/// Resolve font files for a single accumulated font request
	ResolvedUsage ResolveMergedUsage(StyleInfo const& style, std::vector<uint32_t> const& codepoints);
	void ApplyResolvedFontUsage(FileAnalysis& analysis, StyleInfo const& style, UsageData& data, ResolvedUsage const& resolved,
		std::vector<MissingGlyphQuery>& missing_queries);
	void CollectMissingGlyphLines(AssFile const *file, int wrap_style, std::vector<MissingGlyphQuery>& queries);
	void StoreMissingGlyphLines(FontCollectorDetails *details, std::vector<MissingGlyphQuery> const& queries);

	/// Report the ASS styles and line numbers associated with a diagnostic
	void PrintUsage(FontCollectorEventSink const& sink, UsageData const& data);
	void PrintUsage(FontCollectorEventSink const& sink, UsageData const& data, std::vector<int> const& lines);

public:
	/// Constructor
	/// @param status_callback Function to pass status updates to
	/// @param lister The actual font file lister
	FontCollector(FontCollectorEventSink event_sink);
	FontCollector(FontCollectorEventSink event_sink, std::unique_ptr<IFontFileLister> lister);
	FontCollector(FontCollectorEventSink event_sink, IFontFileLister& lister);

	/// @brief Get a list of the locations of all font files used in the file
	/// @param file Lines in the subtitle file to check
	/// @param status Callback function for messages
	/// @return List of paths to fonts
	std::vector<agi::fs::path> GetFontPaths(const AssFile *file, FontCollectorDetails *details = nullptr);
	std::vector<std::vector<agi::fs::path>> GetFontPaths(std::vector<FontCollectorBatchSource> const& sources);
};
