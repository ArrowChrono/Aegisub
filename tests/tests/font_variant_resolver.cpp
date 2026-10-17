#include "../../src/font_variant_resolver.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordingResolver final : public FontVariantResolver {
	FontVariantResolverInfo info;

public:
	std::vector<aegisub::ass::AssFontRequest> requests;
	bool available = true;

	explicit RecordingResolver(
		bool confirms_physical_entity = true,
		bool supports_automatic_pinning = true) {
		info.backend = FontVariantBackend::CoreText;
		info.evidence = FontSelectionEvidence::Observed;
		info.capabilities.confirms_physical_entity = confirms_physical_entity;
		info.capabilities.supports_automatic_pinning = supports_automatic_pinning;
		info.provider = "test-provider";
		info.algorithm = "test-selection";
	}

	FontVariantResolverInfo const& Info() const noexcept override { return info; }
	bool Available() const noexcept override { return available; }

	FontVariantOutcome Resolve(
		aegisub::ass::AssFontRequest const& request) override {
		requests.push_back(request);
		FontVariantOutcome outcome;
		outcome.requested_weight = request.effective_weight;
		outcome.requested_italic = request.italic;
		outcome.realized_weight = request.effective_weight;
		outcome.realized_italic = request.italic;
		outcome.role = request.italic
			? (request.effective_weight == 700
				? FontVariantRole::BoldItalic
				: FontVariantRole::Italic)
			: (request.effective_weight == 700
				? FontVariantRole::Bold
				: FontVariantRole::Regular);
		outcome.status = FontVariantStatus::Canonical;
		outcome.entity_token = requests.size();
		return outcome;
	}
};

FontFamilyRecord Family(FontFamilyId id, std::string localized, std::string english) {
	FontFamilyRecord record;
	record.id = id;
	record.localized_family_name = std::move(localized);
	record.english_win32_family_name = std::move(english);
	return record;
}

} // namespace

TEST(font_variant_resolver, builds_four_canonical_requests_and_preserves_backend) {
	RecordingResolver resolver;
	auto profile = resolver.BuildProfile("Example", 42, 18.25);

	ASSERT_EQ(4u, resolver.requests.size());
	std::array<int, 4> const weights{400, 700, 400, 700};
	std::array<bool, 4> const italics{false, false, true, true};
	for (std::size_t index = 0; index < resolver.requests.size(); ++index) {
		auto const& request = resolver.requests[index];
		EXPECT_EQ("Example", request.family);
		EXPECT_EQ(weights[index], request.effective_weight);
		EXPECT_EQ(italics[index], request.italic);
		EXPECT_EQ(42, request.charset);
		EXPECT_DOUBLE_EQ(18.25, request.height);
	}
	EXPECT_EQ(FontVariantBackend::CoreText, profile.backend);
	EXPECT_EQ(FontSelectionEvidence::Observed, profile.evidence);
	EXPECT_TRUE(profile.automatic_pinning_reliable);
}

TEST(font_variant_resolver, preserves_requested_alias_when_building_live_profile) {
	FontFamilyCatalog catalog({Family(7, "Localized Family", "English Family")});
	aegisub::ass::AssFontRequest request;
	request.family = "English Family";
	request.charset = 128;
	request.height = 22.0;

	RecordingResolver resolver;
	auto profile = BuildFontVariantProfileForRequest(resolver, catalog, request);

	ASSERT_TRUE(profile.has_value());
	ASSERT_EQ(4u, resolver.requests.size());
	for (auto const& probe : resolver.requests) {
		EXPECT_EQ("English Family", probe.family);
		EXPECT_EQ(128, probe.charset);
		EXPECT_DOUBLE_EQ(22.0, probe.height);
	}
}

TEST(font_variant_resolver, same_catalog_family_aliases_are_probed_independently) {
	FontFamilyCatalog catalog({Family(7, "Localized Family", "English Family")});
	aegisub::ass::AssFontRequest request;
	request.family = "English Family";
	RecordingResolver resolver;
	ASSERT_TRUE(BuildFontVariantProfileForRequest(resolver, catalog, request));
	request.family = "Localized Family";
	ASSERT_TRUE(BuildFontVariantProfileForRequest(resolver, catalog, request));

	ASSERT_EQ(8u, resolver.requests.size());
	for (std::size_t index = 0; index < resolver.requests.size(); ++index)
		EXPECT_EQ(index < 4 ? "English Family" : "Localized Family", resolver.requests[index].family);
}

TEST(font_variant_resolver, preserves_vertical_prefix_on_requested_alias_probe) {
	FontFamilyCatalog catalog({Family(7, "Localized Family", "English Family")});
	aegisub::ass::AssFontRequest request;
	request.family = "@English Family";
	request.charset = 128;
	request.height = 19.5;
	RecordingResolver resolver;

	ASSERT_TRUE(BuildFontVariantProfileForRequest(resolver, catalog, request));
	ASSERT_EQ(4u, resolver.requests.size());
	for (auto const& probe : resolver.requests) {
		EXPECT_EQ("@English Family", probe.family);
		EXPECT_EQ(128, probe.charset);
		EXPECT_DOUBLE_EQ(19.5, probe.height);
	}
}

TEST(font_variant_resolver, missing_required_capability_disables_automatic_pin) {
	RecordingResolver no_physical_confirmation(false, true);
	auto unconfirmed = no_physical_confirmation.BuildProfile("Example", 1);
	EXPECT_EQ(FontSelectionEvidence::Observed, unconfirmed.evidence);
	EXPECT_FALSE(unconfirmed.automatic_pinning_reliable);

	RecordingResolver no_automatic_pinning(true, false);
	auto unsupported = no_automatic_pinning.BuildProfile("Example", 1);
	EXPECT_EQ(FontSelectionEvidence::Observed, unsupported.evidence);
	EXPECT_FALSE(unsupported.automatic_pinning_reliable);
}

TEST(font_variant_resolver, unavailable_or_unresolved_request_has_no_profile) {
	FontFamilyCatalog catalog({Family(7, "Localized Family", "English Family")});
	aegisub::ass::AssFontRequest request;
	request.family = "Missing Family";

	RecordingResolver resolver;
	EXPECT_FALSE(BuildFontVariantProfileForRequest(resolver, catalog, request));
	resolver.available = false;
	request.family = "English Family";
	EXPECT_FALSE(BuildFontVariantProfileForRequest(resolver, catalog, request));
	EXPECT_TRUE(resolver.requests.empty());
}
