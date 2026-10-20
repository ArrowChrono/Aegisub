#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_style.h"
#include "../../src/font_file_lister.h"
#include "../../src/font_matching_libass.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordingFontLister final : public IFontFileLister {
public:
	std::vector<aegisub::ass::AssFontRequest> requests;
	std::vector<std::vector<uint32_t>> character_requests;
	int legacy_calls = 0;
	bool return_noncanonical_bold = false;
	bool return_authoritative_noncanonical_bold = false;

	CollectionResult GetFontPaths(
		std::string const&,
		int,
		bool,
		std::vector<uint32_t> const&) override {
		++legacy_calls;
		return {};
	}

	CollectionResult GetFontPaths(
		aegisub::ass::AssFontRequest const& request,
		std::vector<uint32_t> const& characters) override {
		requests.push_back(request);
		character_requests.push_back(characters);
		CollectionResult result;
		result.requested_weight = request.effective_weight;
		result.backend_requested_weight = request.effective_weight;
		if (return_noncanonical_bold) {
			result.matched_weight = 600;
			result.matched_bold = true;
		}
		if (return_authoritative_noncanonical_bold) {
			result.matched_weight = 600;
			result.matched_bold = true;
			result.realized_status = FontVariantStatus::NonCanonical;
		}
		return result;
	}
};

class LegacyFontLister final : public IFontFileLister {
public:
	std::string family;
	int bold = 0;
	bool italic = false;

	CollectionResult GetFontPaths(
		std::string const& requested_family,
		int requested_bold,
		bool requested_italic,
		std::vector<uint32_t> const&) override {
		family = requested_family;
		bold = requested_bold;
		italic = requested_italic;
		return {};
	}
};

class RecordingFontListerProxy final : public IFontFileLister {
	IFontFileLister& inner;

	public:
	std::vector<std::vector<uint32_t>> character_requests;

	explicit RecordingFontListerProxy(IFontFileLister& inner) : inner(inner) {}

	FontFileListerMatchKey GetMatchKey(aegisub::ass::AssFontRequest const& request) const override {
		return inner.GetMatchKey(request);
	}
	CollectionResult GetFontPaths(std::string const& family, int bold, bool italic, std::vector<uint32_t> const& characters) override {
		return inner.GetFontPaths(family, bold, italic, characters);
	}
	CollectionResult GetFontPaths(aegisub::ass::AssFontRequest const& request, std::vector<uint32_t> const& characters) override {
		character_requests.push_back(characters);
		return inner.GetFontPaths(request, characters);
	}
};

class TestLibassProvider final : public ILibassFontProvider {
public:
	std::vector<LibassFontFace> faces;

	TestLibassProvider() {
		LibassFontFace regular;
		regular.families.push_back("Example");
		regular.fullnames.push_back("Example Regular");
		regular.weight = 400;
		faces.push_back(std::move(regular));

		LibassFontFace bold;
		bold.families.push_back("Example");
		bold.fullnames.push_back("Example Bold");
		bold.weight = 700;
		bold.bold = true;
		faces.push_back(std::move(bold));
	}

	std::span<LibassFontFace const> GetLibassFaces() const override {
		return faces;
	}
	bool HasLibassGlyph(size_t, uint32_t) const override { return true; }
	std::vector<std::string> GetLibassSubstitutions(std::string_view) const override { return {}; }
	std::optional<std::string> GetLibassFallback(std::string_view, uint32_t) override { return std::nullopt; }
	std::string_view GetLibassProviderName() const override { return "test"; }
};

AssStyle *AddStyle(
	AssFile& file,
	std::string name,
	std::string family,
	bool bold,
	bool italic,
	int charset,
	double height) {
	auto *style = new AssStyle;
	style->name = std::move(name);
	style->font = std::move(family);
	style->bold = bold;
	style->italic = italic;
	style->encoding = charset;
	style->fontsize = height;
	style->UpdateData();
	file.Styles.push_back(*style);
	return style;
}

void AddLine(AssFile& file, std::string style, std::string text) {
	auto *line = new AssDialogue;
	line->Style = std::move(style);
	line->Text = std::move(text);
	file.Events.push_back(*line);
}

} // namespace

TEST(font_collector_state, rich_request_preserves_vsfilter_state_and_event_baseline) {
	AssFile file;
	AddStyle(file, "Default", "Event Family", true, true, 128, 42.0);
	AddStyle(file, "Reset", "Reset Family", false, false, 204, 24.0);
	AddLine(file, "Default",
		"{\\rReset\\b600\\i0\\fe-1\\fs30}A"
		"{\\t(0,100,\\b2\\i2\\fe)}B");

	RecordingFontLister lister;
	FontCollectorDetails details;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file, &details);

	EXPECT_EQ(0, lister.legacy_calls);
	ASSERT_EQ(2u, lister.requests.size());

	auto first = std::find_if(lister.requests.begin(), lister.requests.end(),
		[](auto const& request) { return request.raw_bold_tag == "600"; });
	ASSERT_NE(lister.requests.end(), first);
	EXPECT_EQ("Reset Family", first->family);
	EXPECT_EQ(600, first->effective_weight);
	EXPECT_FALSE(first->italic);
	EXPECT_EQ(1, first->charset);
	EXPECT_DOUBLE_EQ(30.0, first->height);
	EXPECT_TRUE(first->has_explicit_bold);
	EXPECT_TRUE(first->has_explicit_italic);
	EXPECT_TRUE(first->has_explicit_charset);

	auto transformed = std::find_if(lister.requests.begin(), lister.requests.end(),
		[](auto const& request) { return request.raw_bold_tag == "2"; });
	ASSERT_NE(lister.requests.end(), transformed);
	EXPECT_EQ("Reset Family", transformed->family);
	EXPECT_EQ(700, transformed->effective_weight);
	EXPECT_TRUE(transformed->italic);
	EXPECT_EQ(128, transformed->charset);
	EXPECT_DOUBLE_EQ(30.0, transformed->height);
	EXPECT_TRUE(transformed->has_explicit_charset);
	EXPECT_TRUE(transformed->raw_charset_tag.empty());

	auto usage = std::find_if(details.fonts.begin(), details.fonts.end(),
		[](auto const& item) { return item.ass_raw_bold_tag == "2"; });
	ASSERT_NE(details.fonts.end(), usage);
	EXPECT_EQ(700, usage->ass_effective_weight);
	EXPECT_EQ(128, usage->ass_charset);
	EXPECT_EQ("Event Family", usage->baseline_facename);
	EXPECT_EQ(700, usage->baseline_weight);
	EXPECT_TRUE(usage->baseline_italic);
	EXPECT_EQ(128, usage->baseline_charset);
	EXPECT_DOUBLE_EQ(42.0, usage->baseline_height);
	ASSERT_TRUE(usage->matched.backend_requested_weight.has_value());
	EXPECT_EQ(700, *usage->matched.backend_requested_weight);
}

TEST(font_collector_state, rich_request_adapter_keeps_legacy_listers_compatible) {
	LegacyFontLister lister;
	IFontFileLister& base = lister;

	aegisub::ass::AssFontRequest request;
	request.family = "Example";
	request.effective_weight = 600;
	request.italic = true;
	std::vector<uint32_t> characters{'A'};
	auto result = base.GetFontPaths(request, characters);

	EXPECT_EQ("Example", lister.family);
	EXPECT_EQ(600, lister.bold);
	EXPECT_TRUE(lister.italic);
	ASSERT_TRUE(result.backend_requested_weight.has_value());
	EXPECT_EQ(600, *result.backend_requested_weight);

	request.effective_weight = 400;
	request.has_explicit_bold = true;
	request.raw_bold_tag = "-1";
	result = base.GetFontPaths(request, characters);
	// Generic legacy adapters consume the VSFilter-evaluated state. Libass
	// overrides this entry point and deliberately preserves the raw value.
	EXPECT_EQ(0, lister.bold);
	ASSERT_TRUE(result.backend_requested_weight.has_value());
	EXPECT_EQ(400, *result.backend_requested_weight);
}

TEST(font_collector_state, backend_equivalent_provenance_shares_one_physical_match) {
	AssFile file;
	AddStyle(file, "Default", "Example", false, false, 1, 20.0);
	AddLine(file, "Default", "A{\\b0}B");

	RecordingFontLister lister;
	lister.return_noncanonical_bold = true;
	FontCollectorDetails details;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file, &details);

	ASSERT_EQ(1u, lister.requests.size());
	ASSERT_EQ(1u, lister.character_requests.size());
	EXPECT_EQ((std::vector<uint32_t>{'A', 'B'}), lister.character_requests.front());

	ASSERT_EQ(2u, details.fonts.size());
	auto implicit = std::find_if(details.fonts.begin(), details.fonts.end(),
		[](auto const& usage) { return !usage.ass_has_explicit_bold; });
	auto explicit_regular = std::find_if(details.fonts.begin(), details.fonts.end(),
		[](auto const& usage) {
			return usage.ass_has_explicit_bold && usage.ass_raw_bold_tag == "0";
		});
	ASSERT_NE(details.fonts.end(), implicit);
	ASSERT_NE(details.fonts.end(), explicit_regular);
	EXPECT_EQ((std::vector<uint32_t>{'A'}), implicit->codepoints);
	EXPECT_EQ((std::vector<uint32_t>{'B'}), explicit_regular->codepoints);
	EXPECT_TRUE(implicit->matched.implicit_variant_fallback);
	EXPECT_FALSE(explicit_regular->matched.implicit_variant_fallback);
}

TEST(font_collector_state, legacy_match_key_ignores_unconsumed_ass_font_size) {
	AssFile file;
	AddStyle(file, "Default", "Example", false, false, 1, 20.0);
	AddLine(file, "Default", "{\\fs20}A{\\fs30}B");

	RecordingFontLister lister;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file);

	// The recording lister uses the legacy adapter, which intentionally does not
	// consume height. Platform-specific listers may opt into it when their native
	// selector treats size as a physical-selection input (the Windows GDI lister
	// does so).
	ASSERT_EQ(1u, lister.requests.size());
	EXPECT_EQ(1u, lister.character_requests.size());
	EXPECT_EQ((std::vector<uint32_t>{'A', 'B'}), lister.character_requests.front());
}

TEST(font_collector_state, libass_equivalent_raw_weights_share_one_physical_match) {
	AssFile file;
	AddStyle(file, "Default", "Example", false, false, 1, 20.0);
	AddLine(file, "Default", "{\\b-1}A{\\b1}B");

	FontCollectorEventSink sink;
	LibassFontFileLister backend(sink, std::make_unique<TestLibassProvider>());
	RecordingFontListerProxy lister(backend);
	FontCollectorDetails details;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file, &details);

	ASSERT_EQ(1u, lister.character_requests.size());
	EXPECT_EQ((std::vector<uint32_t>{'A', 'B'}), lister.character_requests.front());
	ASSERT_EQ(2u, details.fonts.size());
	for (auto const& usage : details.fonts) {
		ASSERT_TRUE(usage.matched.backend_requested_weight.has_value());
		EXPECT_EQ(700, *usage.matched.backend_requested_weight);
	}
}

TEST(font_collector_state, libass_semantically_distinct_raw_weights_do_not_merge) {
	AssFile file;
	AddStyle(file, "Default", "Example", false, false, 1, 20.0);
	AddLine(file, "Default", "{\\b-1}A{\\b2}B");

	FontCollectorEventSink sink;
	LibassFontFileLister backend(sink, std::make_unique<TestLibassProvider>());
	RecordingFontListerProxy lister(backend);
	FontCollectorDetails details;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file, &details);

	ASSERT_EQ(2u, lister.character_requests.size());
	std::ranges::sort(lister.character_requests);
	EXPECT_EQ((std::vector<std::vector<uint32_t>>{{'A'}, {'B'}}), lister.character_requests);
	ASSERT_EQ(2u, details.fonts.size());
	auto minus_one = std::find_if(details.fonts.begin(), details.fonts.end(),
		[](auto const& usage) { return usage.ass_raw_bold_tag == "-1"; });
	auto two = std::find_if(details.fonts.begin(), details.fonts.end(),
		[](auto const& usage) { return usage.ass_raw_bold_tag == "2"; });
	ASSERT_NE(details.fonts.end(), minus_one);
	ASSERT_NE(details.fonts.end(), two);
	ASSERT_TRUE(minus_one->matched.backend_requested_weight.has_value());
	ASSERT_TRUE(two->matched.backend_requested_weight.has_value());
	EXPECT_EQ(700, *minus_one->matched.backend_requested_weight);
	EXPECT_EQ(2, *two->matched.backend_requested_weight);
}

TEST(font_collector_state, libass_overload_normalizes_raw_weight_independently) {
	FontCollectorEventSink sink;
	LibassFontFileLister lister(sink, std::make_unique<TestLibassProvider>());

	aegisub::ass::AssFontRequest request;
	request.family = "Example";
	request.effective_weight = 400;
	request.has_explicit_bold = true;
	request.raw_bold_tag = "-1";
	std::vector<uint32_t> characters{'A'};

	auto result = lister.GetFontPaths(request, characters);
	ASSERT_TRUE(result.backend_requested_weight.has_value());
	EXPECT_EQ(700, *result.backend_requested_weight);
	EXPECT_EQ(700, result.matched_weight);

	request.raw_bold_tag = "2";
	result = lister.GetFontPaths(request, characters);
	ASSERT_TRUE(result.backend_requested_weight.has_value());
	EXPECT_EQ(2, *result.backend_requested_weight);
	EXPECT_EQ(400, result.matched_weight);
}

TEST(font_collector_state, libass_lister_reports_canonical_bold_metadata) {
	FontCollectorEventSink sink;
	LibassFontFileLister lister(sink, std::make_unique<TestLibassProvider>());

	aegisub::ass::AssFontRequest request;
	request.family = "Example";
	request.effective_weight = 700;
	auto result = lister.GetFontPaths(request, {'A'});

	EXPECT_EQ(700, result.matched_weight);
	EXPECT_TRUE(result.matched_bold);
	EXPECT_FALSE(result.matched_italic);
	ASSERT_TRUE(result.realized_role.has_value());
	EXPECT_EQ(FontVariantRole::Bold, *result.realized_role);
	ASSERT_TRUE(result.realized_status.has_value());
	EXPECT_EQ(FontVariantStatus::Canonical, *result.realized_status);
	EXPECT_FALSE(result.noncanonical_variant);
}

TEST(font_collector_state, libass_lister_does_not_guess_noncanonical_bold) {
	FontCollectorEventSink sink;

	std::array<std::pair<int, bool>, 2> const metadata_cases{
		std::pair{800, true},
		std::pair{700, false}};
	for (auto const& face_metadata : metadata_cases) {
		auto provider = std::make_unique<TestLibassProvider>();
		provider->faces.clear();
		LibassFontFace face;
		face.families.push_back("Example");
		face.fullnames.push_back("Example Variant");
		face.weight = face_metadata.first;
		face.bold = face_metadata.second;
		provider->faces.push_back(std::move(face));

		LibassFontFileLister lister(sink, std::move(provider));
		aegisub::ass::AssFontRequest request;
		request.family = "Example";
		request.effective_weight = 400;
		auto result = lister.GetFontPaths(request, {'A'});

		EXPECT_FALSE(result.matched_bold);
		ASSERT_TRUE(result.realized_status.has_value());
		EXPECT_EQ(FontVariantStatus::NonCanonical, *result.realized_status);
		EXPECT_FALSE(result.realized_role.has_value());
		EXPECT_TRUE(result.noncanonical_variant);
		EXPECT_FALSE(result.implicit_variant_fallback);
	}
}

TEST(font_collector_state, libass_lister_null_provider_is_safe) {
	std::vector<FontCollectorEvent> events;
	FontCollectorEventSink sink = [&](FontCollectorEvent event) {
		events.push_back(std::move(event));
	};
	LibassFontFileLister lister(
		sink, std::unique_ptr<ILibassFontProvider>{});

	aegisub::ass::AssFontRequest request;
	request.family = "Example";
	request.effective_weight = 700;
	auto result = lister.GetFontPaths(request, {'A'});

	EXPECT_EQ(700, result.requested_weight);
	ASSERT_TRUE(result.backend_requested_weight.has_value());
	EXPECT_EQ(700, *result.backend_requested_weight);
	EXPECT_TRUE(result.paths.empty());
	EXPECT_TRUE(result.memory_fonts.empty());
	ASSERT_FALSE(events.empty());
	EXPECT_EQ(FontCollectorEventType::FontBackendInfo, events.front().type);
}

TEST(font_collector_state, reports_implicit_and_noncanonical_variant_risks) {
	AssFile file;
	AddStyle(file, "Default", "Example", false, false, 1, 20.0);
	AddLine(file, "Default", "A");

	RecordingFontLister lister;
	lister.return_noncanonical_bold = true;
	FontCollectorDetails details;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file, &details);

	ASSERT_EQ(1u, details.fonts.size());
	EXPECT_TRUE(details.fonts.front().matched.implicit_variant_fallback);
	EXPECT_TRUE(details.fonts.front().matched.noncanonical_variant);
}

TEST(font_collector_state, authoritative_noncanonical_result_is_not_implicit_bold) {
	AssFile file;
	AddStyle(file, "Default", "Example", false, false, 1, 20.0);
	AddLine(file, "Default", "A");

	RecordingFontLister lister;
	lister.return_authoritative_noncanonical_bold = true;
	FontCollectorDetails details;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file, &details);

	ASSERT_EQ(1u, details.fonts.size());
	EXPECT_FALSE(details.fonts.front().matched.implicit_variant_fallback);
	EXPECT_TRUE(details.fonts.front().matched.noncanonical_variant);
}

TEST(font_collector_state, transform_controls_preserve_drawing_and_wrap_semantics) {
	AssFile file;
	AddStyle(file, "Default", "Example", false, false, 1, 20.0);
	AddLine(file, "Default", "{\\t(0,100,\\p1)}SHAPE");
	AddLine(file, "Default", "{\\p1\\t(0,100,\\p0)}A");
	AddLine(file, "Default", "{\\t(0,100,\\q2)}C\\nD");

	RecordingFontLister lister;
	FontCollectorDetails details;
	FontCollector collector(FontCollectorEventSink{}, lister);
	collector.GetFontPaths(&file, &details);

	ASSERT_EQ(1u, details.fonts.size());
	auto const& codepoints = details.fonts.front().codepoints;
	EXPECT_NE(codepoints.end(), std::find(codepoints.begin(), codepoints.end(), static_cast<uint32_t>('A')));
	EXPECT_NE(codepoints.end(), std::find(codepoints.begin(), codepoints.end(), static_cast<uint32_t>('C')));
	EXPECT_NE(codepoints.end(), std::find(codepoints.begin(), codepoints.end(), static_cast<uint32_t>('D')));
	EXPECT_EQ(codepoints.end(), std::find(codepoints.begin(), codepoints.end(), static_cast<uint32_t>('S')));
	EXPECT_EQ(codepoints.end(), std::find(codepoints.begin(), codepoints.end(), static_cast<uint32_t>(' ')));
}
