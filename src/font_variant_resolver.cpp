#include "font_variant_resolver.h"

#include <array>
#include <utility>

namespace {

#ifndef _WIN32

class UnavailableFontVariantResolver final : public FontVariantResolver {
	FontVariantResolverInfo info;

public:
	FontVariantResolverInfo const& Info() const noexcept override { return info; }
	bool Available() const noexcept override { return false; }
	FontVariantOutcome Resolve(aegisub::ass::AssFontRequest const&) override {
		return {};
	}
};

#endif

} // namespace

FontFamilyVariantProfile FontVariantResolver::BuildProfile(
	std::string const& family,
	int charset,
	double height) {
	std::array<FontVariantOutcome, 4> outcomes;
	for (std::size_t index = 0; index < outcomes.size(); ++index) {
		aegisub::ass::AssFontRequest request;
		request.family = family;
		request.effective_weight = (index & 1u) != 0
			? aegisub::ass::BoldFontWeight
			: aegisub::ass::DefaultFontWeight;
		request.italic = index >= 2;
		request.charset = charset;
		request.height = height;
		outcomes[index] = Resolve(request);
	}
	auto const& info = Info();
	bool const automatic_pinning_reliable =
		info.evidence == FontSelectionEvidence::Observed &&
		info.capabilities.confirms_physical_entity &&
		info.capabilities.supports_automatic_pinning;
	return BuildFontFamilyVariantProfile(
		std::move(outcomes),
		info.backend,
		info.evidence,
		automatic_pinning_reliable);
}

#ifndef _WIN32
std::unique_ptr<FontVariantResolver> CreatePlatformFontVariantResolver() {
	return std::make_unique<UnavailableFontVariantResolver>();
}
#endif

std::optional<FontFamilyVariantProfile> BuildFontVariantProfileForFamily(
	FontVariantResolver& resolver,
	FontFamilyRecord const& family,
	int charset,
	double height) {
	if (!resolver.Available() || family.localized_family_name.empty())
		return std::nullopt;
	return resolver.BuildProfile(family.localized_family_name, charset, height);
}

std::optional<FontFamilyVariantProfile> BuildFontVariantProfileForRequest(
	FontVariantResolver& resolver,
	FontFamilyCatalog const& catalog,
	aegisub::ass::AssFontRequest const& request) {
	if (!resolver.Available() || !request.valid)
		return std::nullopt;
	auto const resolved = catalog.Resolve(request.family);
	if (!resolved.family)
		return std::nullopt;
	auto const* record = catalog.Find(*resolved.family);
	if (!record)
		return std::nullopt;
	return resolver.BuildProfile(request.family, request.charset, request.height);
}
