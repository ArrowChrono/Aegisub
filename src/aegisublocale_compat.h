#pragma once

#include <wx/arrstr.h>
#include <wx/uilocale.h>

namespace aegisub::locale {

inline wxVector<wxString> ToVector(wxArrayString const& values) {
	wxVector<wxString> result;
	result.reserve(values.size());
	for (auto const& value : values)
		result.push_back(value);
	return result;
}

inline wxString FindPreferredTranslation(wxArrayString const& supported, wxVector<wxString> const& preferred_languages) {
	if (supported.empty())
		return {};

	auto supported_vec = ToVector(supported);

	auto try_match = [&](wxString const& candidate) -> wxString {
		if (candidate.empty())
			return {};
		#if wxCHECK_VERSION(3, 3, 0)
		return wxLocaleIdent::GetBestMatch(candidate, supported_vec);
#else
		// wxWidgets 3.2 has locale parsing but no GetBestMatch helper.
		for (auto const& available : supported_vec)
			if (available.CmpNoCase(candidate) == 0)
				return available;
		auto requested = wxLocaleIdent::FromTag(candidate);
		if (requested.IsEmpty()) return {};
		for (auto const& available : supported_vec) {
			auto locale = wxLocaleIdent::FromTag(available);
			if (!locale.IsEmpty() && locale.GetLanguage() == requested.GetLanguage()
				&& locale.GetRegion() == requested.GetRegion()
				&& locale.GetModifier() == requested.GetModifier()
				&& locale.GetScript() == requested.GetScript())
				return available;
		}
		return {};
#endif
	};

	for (auto const& preferred : preferred_languages) {
		auto locale_id = wxLocaleIdent::FromTag(preferred);

		if (!locale_id.IsEmpty()) {
			if (auto match = try_match(locale_id.GetTag(wxLOCALE_TAGTYPE_POSIX)); !match.empty())
				return match;
			if (auto match = try_match(locale_id.GetTag(wxLOCALE_TAGTYPE_BCP47)); !match.empty())
				return match;

			auto language = locale_id.GetLanguage();
			auto region = locale_id.GetRegion();
			if (!language.empty()) {
				if (!region.empty()) {
					if (auto match = try_match(language + wxS("_") + region); !match.empty())
						return match;
				}
				if (auto match = try_match(language); !match.empty())
					return match;
			}
		}

		if (auto match = try_match(preferred); !match.empty())
			return match;
	}

	return {};
}

inline wxString FindPreferredTranslation(wxArrayString const& supported) {
	return FindPreferredTranslation(supported, wxUILocale::GetPreferredUILanguages());
}

} // namespace aegisub::locale
