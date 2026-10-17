#include "font_file_lister.h"

#include "font_collector_unicode.h"
#include "font_matching_common.h"
#include "font_matching_libass.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <utility>

namespace {
void Emit(FontCollectorEventSink const& sink, FontCollectorEvent event) {
	if (sink)
		sink(event);
}

std::string MatchSourceName(LibassFontMatchSource source) {
	switch (source) {
		case LibassFontMatchSource::Requested: return "requested";
		case LibassFontMatchSource::Default: return "default";
		case LibassFontMatchSource::Fallback: return "fallback";
	}
	return "requested";
}

std::string NameMatchName(LibassFontNameMatch match) {
	switch (match) {
		case LibassFontNameMatch::Family: return "family";
		case LibassFontNameMatch::ExtendedFamily: return "extended_family";
		case LibassFontNameMatch::FullName: return "full_name";
		case LibassFontNameMatch::PostScript: return "postscript";
		case LibassFontNameMatch::FullNameAndPostScript: return "full_name_and_postscript";
	}
	return "family";
}

struct LibassVariantMetadata {
	FontVariantRole role = FontVariantRole::Unknown;
	FontVariantStatus status = FontVariantStatus::Unknown;
	bool matched_bold = false;
	bool matched_italic = false;
};

LibassVariantMetadata ClassifyLibassVariant(
	LibassFontFace const& face,
	bool fake_bold,
	bool fake_italic) {
	LibassVariantMetadata result;
	bool const canonical_weight =
		face.weight == aegisub::ass::DefaultFontWeight ||
		face.weight == aegisub::ass::BoldFontWeight;
	bool const expected_bold = face.weight == aegisub::ass::BoldFontWeight;
	// Italic is useful as a diagnostic even when weight metadata makes the
	// variant non-canonical or synthetic. Automatic pinning still requires the
	// canonical status below.
	result.matched_italic = face.italic;
	bool const metadata_consistent = canonical_weight && face.bold == expected_bold;
	if (!metadata_consistent) {
		// Numeric weights outside the ASS RBIZ pair and conflicting OS/2/style
		// metadata are diagnostic-only. In particular, never turn 800/900 into
		// a boolean Bold result for the collector's implicit-fallback heuristic.
		result.status = FontVariantStatus::NonCanonical;
		return result;
	}
	if (fake_bold || fake_italic) {
		result.status = FontVariantStatus::Synthetic;
		return result;
	}

	bool const bold = expected_bold;
	if (bold && face.italic)
		result.role = FontVariantRole::BoldItalic;
	else if (bold)
		result.role = FontVariantRole::Bold;
	else if (face.italic)
		result.role = FontVariantRole::Italic;
	else
		result.role = FontVariantRole::Regular;
	result.status = FontVariantStatus::Canonical;
	result.matched_bold = bold;
	return result;
}

void SetPrimaryMatch(CollectionResult& result, LibassFontFace const& face,
					 std::string const& requested_name, LibassFontRequest const& request,
					 std::string_view provider_name) {
	if (!face.path.empty()) result.primary_path = agi::fs::PathFromString(face.path);
	result.matched_facename = face.families.empty() ? requested_name : face.families.front();
	result.matched_facename_full = face.fullnames.empty() ? std::string() : face.fullnames.front();
	result.matched_names = face.families;
	result.face_index = face.face_index;
	result.matched_weight = face.weight;
	result.fake_bold = request.weight > face.weight + 150 && !face.bold;
	result.fake_italic = request.italic && !face.italic;
	auto const variant = ClassifyLibassVariant(face, result.fake_bold, result.fake_italic);
	result.matched_bold = variant.matched_bold;
	result.matched_italic = variant.matched_italic;
	result.realized_status = variant.status;
	if (variant.role != FontVariantRole::Unknown) result.realized_role = variant.role;
	result.noncanonical_variant = variant.status != FontVariantStatus::Canonical;
	result.path_source = "libass-" + std::string(provider_name);
}

void AddFontSource(CollectionResult& result, ILibassFontProvider const& provider,
				   size_t index, bool primary, std::string const& requested_name) {
	auto const& face = provider.GetLibassFaces()[index];
	if (face.path.empty()) {
		auto data = provider.GetLibassFontData(index);
		if (data && !data->empty()) {
			if (primary) result.primary_memory_data = data;
			FontMemoryFont memory_font;
			memory_font.facename = !face.fullnames.empty() ? face.fullnames.front() : !face.families.empty() ? face.families.front()
																											 : requested_name;
			memory_font.data = std::move(data);
			result.memory_fonts.push_back(std::move(memory_font));
		}
		return;
	}
	auto path = agi::fs::PathFromString(face.path);
	if (std::ranges::find(result.paths, path) == result.paths.end())
		result.paths.push_back(std::move(path));
}
}

LibassFontFileLister::LibassFontFileLister(
	FontCollectorEventSink& event_sink,
	std::unique_ptr<ILibassFontProvider> provider,
	bool collect_match_candidates)
	: provider(std::move(provider)), collect_match_candidates(collect_match_candidates) {
	FontCollectorEvent event;
	event.type = FontCollectorEventType::FontBackendInfo;
	event.message = this->provider
		? "libass selector (" + std::string(this->provider->GetLibassProviderName()) + " provider)"
		: "libass selector (unavailable provider)";
	Emit(event_sink, std::move(event));
}

LibassFontFileLister::~LibassFontFileLister() = default;

FontFileListerMatchKey LibassFontFileLister::GetMatchKey(
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

CollectionResult LibassFontFileLister::GetFontPaths(
	std::string const& facename,
	int bold,
	bool italic,
	std::vector<uint32_t> const& characters) {
	CollectionResult result;
	auto normalized = NormalizeLibassFontRequest(facename, bold, italic);
	LibassFontRequest request;
	request.family = std::move(normalized.facename);
	request.weight = normalized.requested_weight;
	request.italic = normalized.requested_italic;
	request.default_family = "Sans";
	result.requested_weight = request.weight;
	result.backend_requested_weight = request.weight;
	if (!provider)
		return result;

	auto const faces = provider->GetLibassFaces();
	if (faces.empty())
		return result;

	auto selection = SelectLibassFontFaces(
		faces,
		request,
		characters,
		[this](size_t index, uint32_t codepoint) {
			return provider->HasLibassGlyph(index, codepoint);
		},
		[this](std::string_view family) {
			return provider->GetLibassSubstitutions(family);
		},
		[this](std::string_view family, uint32_t codepoint) {
			return provider->GetLibassFallback(family, codepoint);
		},
		collect_match_candidates);

	result.match_ambiguous = selection.ambiguous;
	result.match_candidates.reserve(selection.candidates.size());
	for (auto const& candidate_match : selection.candidates) {
		if (candidate_match.face >= faces.size())
			continue;
		auto const& face = faces[candidate_match.face];
		auto& candidate = result.match_candidates.emplace_back();
		candidate.facename = face.families.empty() ? std::string() : face.families.front();
		candidate.facename_full = face.fullnames.empty() ? std::string() : face.fullnames.front();
		candidate.matched_name = candidate_match.matched_name;
		candidate.match_source = MatchSourceName(candidate_match.source);
		candidate.name_match = NameMatchName(candidate_match.name_match);
		candidate.path = face.path;
		candidate.provider_order = static_cast<int>(candidate_match.face);
		candidate.face_index = face.face_index;
		candidate.score = candidate_match.score;
		candidate.weight = face.weight;
		candidate.bold = face.bold;
		candidate.italic = face.italic;
		candidate.considered_codepoints = candidate_match.considered_codepoints;
		candidate.supported_codepoints = candidate_match.supported_codepoints;
		candidate.selected_codepoints = candidate_match.selected_codepoints;
	}

	for (auto codepoint : selection.missing_codepoints) {
		font_collector::unicode::Rune rune;
		if (font_collector::unicode::Rune::TryCreate(codepoint, rune))
			font_collector::unicode::AppendRuneToUtf8(result.missing, rune);
	}

	if (selection.faces.empty()) {
		auto const ranking = RankLibassFamilyFaces(faces, request,
												   [this](std::string_view family) { return provider->GetLibassSubstitutions(family); });
		if (ranking.face) {
			auto family = std::make_shared<CollectionResult>(result);
			family->match_ambiguous = ranking.ambiguous;
			SetPrimaryMatch(*family, faces[*ranking.face], facename, request, provider->GetLibassProviderName());
			AddFontSource(*family, *provider, *ranking.face, true, facename);
			result.glyphless_family = std::move(family);
		}
		return result;
	}

	auto const& first = faces[selection.faces.front()];
	SetPrimaryMatch(result, first, facename, request, provider->GetLibassProviderName());
	for (auto index : selection.faces)
		AddFontSource(result, *provider, index, index == selection.faces.front(), facename);

	return result;
}

CollectionResult LibassFontFileLister::GetFontPaths(
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
