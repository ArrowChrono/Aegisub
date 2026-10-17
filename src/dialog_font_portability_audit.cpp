#include "ass_file.h"
#include "ass_font_state.h"
#include "compat.h"
#include "dialog_manager.h"
#include "font_family_catalog.h"
#include "font_family_catalog_cache.h"
#include "font_portability_audit.h"
#include "font_variant_resolver.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {

enum class FindingKind {
	Name,
	Variant
};

struct VisibleFinding {
	FindingKind kind;
	std::size_t plan_index;
};

FontVariantAuditProfileProvider MakeLiveProfileProvider(
	FontVariantResolver& resolver,
	FontFamilyCatalog const& catalog) {
	return [&resolver, &catalog](aegisub::ass::AssFontRequest const& request)
		-> std::optional<FontFamilyVariantProfile> {
		return BuildFontVariantProfileForRequest(resolver, catalog, request);
	};
}

bool Includes(FontPortabilityAuditFilter filter, FindingKind kind) {
	return filter == FontPortabilityAuditFilter::All ||
	       (filter == FontPortabilityAuditFilter::Names && kind == FindingKind::Name) ||
	       (filter == FontPortabilityAuditFilter::Variants && kind == FindingKind::Variant);
}

wxString MatchKindText(FontFamilyMatchKind kind) {
	switch (kind) {
		case FontFamilyMatchKind::None: return _("No match");
		case FontFamilyMatchKind::Exact: return _("Exact");
		case FontFamilyMatchKind::CaseInsensitiveExact: return _("Case-insensitive exact");
		case FontFamilyMatchKind::Ambiguous: return _("Ambiguous");
	}
	return _("Unknown");
}

wxString NameKindText(FontFamilyNameKind kind) {
	switch (kind) {
		case FontFamilyNameKind::Win32Family: return _("Win32 family");
		case FontFamilyNameKind::TypographicFamily: return _("Typographic family");
		case FontFamilyNameKind::FullName: return _("Full name");
		case FontFamilyNameKind::PostScript: return _("PostScript");
		case FontFamilyNameKind::PlatformAlias: return _("Platform alias");
	}
	return _("Unknown");
}

wxString NameReasonText(std::string const& reason) {
	if (reason == "font_family_catalog_unavailable")
		return _("The installed font family catalog is unavailable.");
	if (reason == "ambiguous_family_alias")
		return _("The name matches more than one installed font family.");
	if (reason == "unrecognized_family_name")
		return _("The name is not a confirmed Win32 family name for an installed font.");
	if (reason == "english_win32_name_unavailable")
		return _("This family has no English Win32 family name.");
	if (reason == "preferred_family_name_unavailable")
		return _("The selected target name is unavailable for this family.");
	if (reason == "gdi_family_name_too_long")
		return _("The target name exceeds the GDI family-name length limit.");
	if (reason == "noncanonical_family_name_case")
		return _("The family name uses noncanonical letter case.");
	if (reason == "localized_win32_family_alias")
		return _("Replace the localized Win32 family name with its English form.");
	if (reason == "english_win32_family_alias")
		return _("Replace the English Win32 family name with its localized form.");
	if (reason == "win32_family_alias")
		return _("Replace this confirmed Win32 family alias with the selected target name.");
	if (reason == "font_state_preservation_unproven")
		return _("Font selection cannot be proven unchanged for all affected text; report only, not applied automatically.");
	return to_wx(reason);
}

wxString NameSourceText(FontNameSourceLocation const& source) {
	if (source.kind == FontNameSourceKind::Style)
		return _("Style") + wxS(" \"") + to_wx(source.style) + wxS("\"");

	auto text = source.comment ? _("Comment line ") : _("Dialogue line ");
	text += wxString::Format(wxS("%d"), source.line);
	text += wxS(", \\fn #");
	text += wxString::Format(wxS("%llu"),
		static_cast<unsigned long long>(source.override_index + 1));
	if (!source.style.empty())
		text += wxS(" (") + _("style") + wxS(" \"") + to_wx(source.style) + wxS("\")");
	return text;
}

wxString ClassificationText(FontVariantAuditClassification classification) {
	switch (classification) {
		case FontVariantAuditClassification::ImplicitBoldFallback:
			return _("Implicit Bold fallback");
		case FontVariantAuditClassification::ImplicitItalicFallback:
			return _("Implicit Italic fallback");
		case FontVariantAuditClassification::ImplicitBoldItalicFallback:
			return _("Implicit Bold Italic fallback");
		case FontVariantAuditClassification::NonCanonical:
			return _("Noncanonical variant");
		case FontVariantAuditClassification::Synthetic:
			return _("Synthetic variant");
		case FontVariantAuditClassification::Unknown:
			return _("Unknown variant");
	}
	return _("Unknown variant");
}

wxString StatusText(FontVariantStatus status) {
	switch (status) {
		case FontVariantStatus::Canonical: return _("Canonical");
		case FontVariantStatus::NonCanonical: return _("Noncanonical");
		case FontVariantStatus::Synthetic: return _("Synthetic");
		case FontVariantStatus::Unknown: return _("Unknown");
	}
	return _("Unknown");
}

wxString RoleText(FontVariantRole role) {
	switch (role) {
		case FontVariantRole::Regular: return _("Regular");
		case FontVariantRole::Bold: return _("Bold");
		case FontVariantRole::Italic: return _("Italic");
		case FontVariantRole::BoldItalic: return _("Bold Italic");
		case FontVariantRole::Unknown: return _("Unknown");
	}
	return _("Unknown");
}

wxString VariantSourceText(FontVariantAuditSource const& source) {
	if (source.kind == FontVariantAuditSourceKind::Style) {
		auto text = _("Style") + wxS(" \"") + to_wx(source.style) + wxS("\"");
		if (source.line > 0)
			text += wxString::Format(wxS(" (%s %d)"), _("line"), source.line);
		return text;
	}

	auto text = source.comment ? _("Comment line ") : _("Dialogue line ");
	text += wxString::Format(wxS("%d"), source.line);
	text += wxS(", \\fn #");
	text += wxString::Format(wxS("%llu"),
		static_cast<unsigned long long>(source.fn_override_index + 1));
	if (!source.style.empty())
		text += wxS(" (") + _("style") + wxS(" \"") + to_wx(source.style) + wxS("\")");
	return text;
}

wxString VariantReasonText(std::string const& reason) {
	if (reason == "implicit_bold_fallback")
		return _("The Normal request resolves to the family's native Bold face.");
	if (reason == "implicit_italic_fallback")
		return _("The non-Italic request resolves to the family's native Italic face.");
	if (reason == "implicit_bold_italic_fallback")
		return _("The ordinary request resolves to the family's native Bold Italic face.");
	if (reason == "explicit_variant_override_preserved")
		return _("An explicit \\b or \\i override is preserved unless replacement is enabled.");
	if (reason == "numeric_weight_report_only")
		return _("An exact numeric \\b weight cannot be verified from the RBIZ family profile.");
	if (reason == "transform_variant_report_only")
		return _("Font state originating in a transform is report-only.");
	if (reason == "synthetic_variant")
		return _("The active font backend synthesizes this requested variant.");
	if (reason == "noncanonical_variant")
		return _("The selected backend result cannot be represented safely as an RBIZ fix.");
	if (reason == "font_state_preservation_unproven")
		return _("Font selection cannot be proven unchanged for all affected text; report only, not applied automatically.");
	if (reason == "variant_profile_unavailable")
		return _("The installed family has no reliable variant profile for this request.");
	if (reason == "font_family_catalog_unavailable")
		return _("The installed font family catalog is unavailable.");
	if (reason == "ambiguous_family_alias")
		return _("The font name matches more than one installed family.");
	if (reason == "unrecognized_family_name")
		return _("The font name does not resolve to an installed family.");
	return to_wx(reason);
}

class DialogFontPortabilityAudit : public wxDialog {
	agi::Context* context;
	std::shared_ptr<FontFamilyCatalog const> catalog;
	FontPortabilityAuditPlan plan;
	FontPortabilityAuditSelection audit_selection;
	std::vector<VisibleFinding> visible_findings;

	wxChoice* filter_box;
	wxRadioBox* target_box;
	wxCheckBox* allow_explicit;
	wxCheckListBox* findings;
	wxStaticText* summary;
	wxTextCtrl* details;
	wxButton* apply_button;

	FontPortabilityAuditFilter CurrentFilter() const;
	FontPortabilityAuditOptions CurrentOptions() const;
	FontFamilyCatalog const& CurrentCatalog() const;
	void BuildInitialPlan();
	void RebuildNames();
	void RebuildVariants();
	void RebuildVisibleFindings();
	void UpdateSummary();
	void UpdateDetails();
	void UpdateApplyButton();
	void SelectSafe(bool select);
	void OnCheck(wxCommandEvent& event);
	void OnApply(wxCommandEvent& event);

	bool IsSafe(VisibleFinding const& finding) const;
	bool IsSelected(VisibleFinding const& finding) const;
	void SetSelected(VisibleFinding const& finding, bool selected);

public:
	DialogFontPortabilityAudit(
		agi::Context* c,
		FontPortabilityAuditFilter initial_filter);
};

DialogFontPortabilityAudit::DialogFontPortabilityAudit(
	agi::Context* c,
	FontPortabilityAuditFilter initial_filter)
: wxDialog(c->GetUI().parent, -1, _("Audit Font Portability"),
	  wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, context(c)
, catalog(font_family_catalog_cache::TryGetSnapshot())
{
	SetIcon(GETICON(font_collector_button_16));

	wxString filters[] = {
		_("All findings"),
		_("Family names"),
		_("Variants")
	};
	filter_box = new wxChoice(
		this, -1, wxDefaultPosition, FromDIP(wxSize(190, -1)), 3, filters);
	filter_box->SetSelection(static_cast<int>(initial_filter));

	wxString targets[] = {
		_("Localized family names"),
		_("English Win32 family names")
	};
	target_box = new wxRadioBox(
		this, -1, _("Target names"), wxDefaultPosition, wxDefaultSize,
		2, targets, 1, wxRA_SPECIFY_ROWS);
	bool prefer_localized = true;
	try {
		prefer_localized = OPT_GET("Subtitle/Font/Prefer Localized Family Names")->GetBool();
	}
	catch (...) {
	}
	target_box->SetSelection(prefer_localized ? 0 : 1);

	allow_explicit = new wxCheckBox(
		this, -1, _("Allow replacing explicit \\b/\\i overrides"));
	summary = new wxStaticText(this, -1, wxString{});
	findings = new wxCheckListBox(
		this, -1, wxDefaultPosition, FromDIP(wxSize(720, 250)),
		0, nullptr, wxLB_HSCROLL | wxLB_SINGLE);
	details = new wxTextCtrl(
		this, -1, wxString{}, wxDefaultPosition, FromDIP(wxSize(720, 180)),
		wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_DONTWRAP);

	auto* filter_sizer = new wxBoxSizer(wxVERTICAL);
	filter_sizer->Add(new wxStaticText(this, -1, _("Show")),
		wxSizerFlags().Border(wxBOTTOM, 3));
	filter_sizer->Add(filter_box);

	auto* controls = new wxBoxSizer(wxHORIZONTAL);
	controls->Add(filter_sizer, wxSizerFlags().Border());
	controls->Add(target_box, wxSizerFlags().Border());

	auto* select_safe = new wxButton(this, -1, _("Select &Safe"));
	auto* clear = new wxButton(this, -1, _("&Clear"));
	auto* selection_buttons = new wxBoxSizer(wxHORIZONTAL);
	selection_buttons->Add(select_safe, wxSizerFlags().Border(wxRIGHT));
	selection_buttons->Add(clear);

	auto* button_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
	apply_button = button_sizer->GetAffirmativeButton();
	apply_button->SetLabel(_("&Apply Selected"));

	auto* main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(controls, wxSizerFlags().Expand());
	main_sizer->Add(allow_explicit, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(summary, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(findings, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));
	main_sizer->Add(selection_buttons, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxTOP));
	main_sizer->Add(new wxStaticText(this, -1, _("Details")),
		wxSizerFlags().Border(wxLEFT | wxRIGHT | wxTOP));
	main_sizer->Add(details, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));
	main_sizer->Add(button_sizer, wxSizerFlags().Right().Border());

	SetSizerAndFit(main_sizer);
	SetMinSize(FromDIP(wxSize(660, 600)));
	SetSize(FromDIP(wxSize(800, 700)));
	CenterOnParent();

	filter_box->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { RebuildVisibleFindings(); });
	target_box->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { RebuildNames(); });
	allow_explicit->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { RebuildVariants(); });
	findings->Bind(wxEVT_CHECKLISTBOX, &DialogFontPortabilityAudit::OnCheck, this);
	findings->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { UpdateDetails(); });
	select_safe->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SelectSafe(true); });
	clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SelectSafe(false); });
	Bind(wxEVT_BUTTON, &DialogFontPortabilityAudit::OnApply, this, wxID_OK);

	BuildInitialPlan();
}

FontPortabilityAuditFilter DialogFontPortabilityAudit::CurrentFilter() const {
	switch (filter_box->GetSelection()) {
		case 1: return FontPortabilityAuditFilter::Names;
		case 2: return FontPortabilityAuditFilter::Variants;
		default: return FontPortabilityAuditFilter::All;
	}
}

FontPortabilityAuditOptions DialogFontPortabilityAudit::CurrentOptions() const {
	FontPortabilityAuditOptions options;
	options.name_target = target_box->GetSelection() == 0
		? FontNameNormalizationTarget::Localized
		: FontNameNormalizationTarget::EnglishWin32;
	options.variant.allow_replace_explicit = allow_explicit->GetValue();
	return options;
}

FontFamilyCatalog const& DialogFontPortabilityAudit::CurrentCatalog() const {
	static FontFamilyCatalog const empty_catalog;
	return catalog ? *catalog : empty_catalog;
}

void DialogFontPortabilityAudit::BuildInitialPlan() {
	auto const options = CurrentOptions();
	auto resolver = CreatePlatformFontVariantResolver();
	auto live_profiles = MakeLiveProfileProvider(*resolver, CurrentCatalog());
	plan = BuildFontPortabilityAuditPlan(
		*context->GetCore().ass, CurrentCatalog(), options, {}, live_profiles);
	audit_selection.Reset(plan, CurrentFilter());
	RebuildVisibleFindings();
}

void DialogFontPortabilityAudit::RebuildNames() {
	auto const options = CurrentOptions();
	plan.options.name_target = options.name_target;
	auto resolver = CreatePlatformFontVariantResolver();
	auto live_profiles = MakeLiveProfileProvider(*resolver, CurrentCatalog());
	plan.names = BuildFontPortabilityAuditPlan(*context->GetCore().ass, CurrentCatalog(), options, {}, live_profiles).names;
	audit_selection.ResetNames(
		plan.names, Includes(CurrentFilter(), FindingKind::Name));
	RebuildVisibleFindings();
}

void DialogFontPortabilityAudit::RebuildVariants() {
	auto const options = CurrentOptions();
	plan.options.variant = options.variant;
	auto resolver = CreatePlatformFontVariantResolver();
	auto live_profiles = MakeLiveProfileProvider(*resolver, CurrentCatalog());
	plan.variants = BuildFontVariantAuditPlan(
		*context->GetCore().ass, CurrentCatalog(), options.variant, {}, live_profiles);
	audit_selection.ResetVariants(
		plan.variants, Includes(CurrentFilter(), FindingKind::Variant));
	RebuildVisibleFindings();
}

bool DialogFontPortabilityAudit::IsSafe(VisibleFinding const& finding) const {
	if (finding.kind == FindingKind::Name)
		return finding.plan_index < plan.names.changes.size() &&
		       plan.names.changes[finding.plan_index].safe_to_apply;
	return finding.plan_index < plan.variants.findings.size() &&
	       plan.variants.findings[finding.plan_index].safe_to_apply;
}

bool DialogFontPortabilityAudit::IsSelected(VisibleFinding const& finding) const {
	if (finding.kind == FindingKind::Name)
		return audit_selection.IsNameSelected(finding.plan_index);
	return audit_selection.IsVariantSelected(finding.plan_index);
}

void DialogFontPortabilityAudit::SetSelected(
	VisibleFinding const& finding,
	bool selected) {
	if (finding.kind == FindingKind::Name)
		audit_selection.SetNameSelected(plan.names, finding.plan_index, selected);
	else
		audit_selection.SetVariantSelected(plan.variants, finding.plan_index, selected);
}

void DialogFontPortabilityAudit::RebuildVisibleFindings() {
	visible_findings.clear();
	visible_findings.reserve(plan.names.changes.size() + plan.variants.findings.size());

	findings->Freeze();
	findings->Clear();
	auto const filter = CurrentFilter();
	target_box->Enable(Includes(filter, FindingKind::Name));
	allow_explicit->Enable(Includes(filter, FindingKind::Variant));
	if (Includes(filter, FindingKind::Name)) {
		for (std::size_t i = 0; i < plan.names.changes.size(); ++i) {
			auto const& change = plan.names.changes[i];
			auto row = wxS("[") + _("Family name") + wxS("] ") +
				NameSourceText(change.source) + wxS(": ") + to_wx(change.current_name);
			if (!change.recommended_name.empty())
				row += wxS(" -> ") + to_wx(change.recommended_name);
			else
				row += wxS(" [") + _("Needs review") + wxS("]");
			auto const row_index = findings->Append(row);
			visible_findings.push_back({FindingKind::Name, i});
			findings->Check(row_index, audit_selection.IsNameSelected(i));
		}
	}
	if (Includes(filter, FindingKind::Variant)) {
		for (std::size_t i = 0; i < plan.variants.findings.size(); ++i) {
			auto const& finding = plan.variants.findings[i];
			auto row = wxS("[") + _("Variant") + wxS("] ") +
				VariantSourceText(finding.source) + wxS(": ") + to_wx(finding.family) +
				wxS(" - ") + ClassificationText(finding.classification);
			if (!finding.safe_to_apply)
				row += wxS(" [") + _("Review only") + wxS("]");
			auto const row_index = findings->Append(row);
			visible_findings.push_back({FindingKind::Variant, i});
			findings->Check(row_index, audit_selection.IsVariantSelected(i));
		}
	}
	findings->Thaw();

	if (!visible_findings.empty())
		findings->SetSelection(0);
	UpdateDetails();
	UpdateApplyButton();
	Layout();
}

void DialogFontPortabilityAudit::UpdateSummary() {
	auto const name_safe = static_cast<std::size_t>(std::count_if(
		plan.names.changes.begin(), plan.names.changes.end(),
		[](auto const& change) { return change.safe_to_apply; }));
	auto const variant_safe = static_cast<std::size_t>(std::count_if(
		plan.variants.findings.begin(), plan.variants.findings.end(),
		[](auto const& finding) { return finding.safe_to_apply; }));
	auto const name_selected = audit_selection.SelectedNameCount();
	auto const variant_selected = audit_selection.SelectedVariantCount();

	wxString status;
	switch (CurrentFilter()) {
		case FontPortabilityAuditFilter::Names:
			status = wxString::Format(
				_("Scanned names: %llu    Safe changes: %llu    Issues: %llu"),
				static_cast<unsigned long long>(plan.names.scanned_name_count),
				static_cast<unsigned long long>(name_safe),
				static_cast<unsigned long long>(plan.names.changes.size() - name_safe));
			break;
		case FontPortabilityAuditFilter::Variants:
			status = wxString::Format(
				_("Scanned styles: %llu    Explicit font spans: %llu    Safe fixes: %llu    Review: %llu"),
				static_cast<unsigned long long>(plan.variants.scanned_style_count),
				static_cast<unsigned long long>(plan.variants.scanned_override_span_count),
				static_cast<unsigned long long>(variant_safe),
				static_cast<unsigned long long>(plan.variants.findings.size() - variant_safe));
			break;
		case FontPortabilityAuditFilter::All:
			status = wxString::Format(
				_("Font names: %llu findings, %llu safe    Variants: %llu findings, %llu safe"),
				static_cast<unsigned long long>(plan.names.changes.size()),
				static_cast<unsigned long long>(name_safe),
				static_cast<unsigned long long>(plan.variants.findings.size()),
				static_cast<unsigned long long>(variant_safe));
			break;
	}
	status += wxS("\n") + wxString::Format(
		_("Selected fixes: %llu"),
		static_cast<unsigned long long>(name_selected + variant_selected));
	if (!plan.names.catalog_available || !plan.variants.catalog_available)
		status += wxS("\n") +
			_("The installed font family catalog is unavailable; changes cannot be applied.");
	summary->SetLabel(status);
}

void DialogFontPortabilityAudit::UpdateDetails() {
	auto const selection = findings->GetSelection();
	if (selection == wxNOT_FOUND ||
	    static_cast<std::size_t>(selection) >= visible_findings.size()) {
		details->SetValue(visible_findings.empty() ? _("No font portability findings.") : wxString{});
		return;
	}

	auto const visible = visible_findings[selection];
	wxString text;
	if (visible.kind == FindingKind::Name) {
		auto const& change = plan.names.changes[visible.plan_index];
		text += _("Category: Family name\n");
		text += _("Source: ") + NameSourceText(change.source) + wxS("\n");
		text += _("Current name: ") + to_wx(change.current_name) + wxS("\n");
		text += _("Recommended name: ") +
			(change.recommended_name.empty() ? _("Not available") : to_wx(change.recommended_name)) + wxS("\n");
		text += _("Status: ") +
			(change.safe_to_apply ? _("Safe to apply") : _("Review only")) + wxS("\n");
		text += _("Match: ") + MatchKindText(change.match_kind) + wxS("\n");
		text += _("Reason: ") + NameReasonText(change.reason_code) + wxS("\n");
		text += _("Rule: ") + to_wx(change.reason_code);

		if (catalog && !catalog->empty()) {
			auto const resolved = catalog->Resolve(change.current_name);
			if (resolved.family) {
				if (auto const* record = catalog->Find(*resolved.family)) {
					text += wxS("\n\n") + _("Installed family information") + wxS("\n");
					text += _("Localized family name: ") +
						to_wx(record->localized_family_name) + wxS("\n");
					text += _("English Win32 family name: ") +
						(record->english_win32_family_name.empty()
							? _("Not available")
							: to_wx(record->english_win32_family_name));
					for (auto const& name : record->names) {
						text += wxS("\n") + NameKindText(name.kind);
						if (!name.locale.empty())
							text += wxS(" [") + to_wx(name.locale) + wxS("]");
						text += wxS(": ") + to_wx(name.value);
					}
				}
			}
		}
	}
	else {
		auto const& finding = plan.variants.findings[visible.plan_index];
		text += _("Category: Variant\n");
		text += _("Source: ") + VariantSourceText(finding.source) + wxS("\n");
		text += _("Family: ") + to_wx(finding.family) + wxS("\n");
		text += _("Classification: ") + ClassificationText(finding.classification) + wxS("\n");
		text += _("Variant profile status: ") + StatusText(finding.status) + wxS("\n");
		text += _("Physical role: ") + RoleText(finding.realized_role) + wxS("\n");
		text += wxString::Format(_("ASS request: weight %d, italic %s, size %.2f\n"),
			finding.requested_weight, finding.requested_italic ? _("yes") : _("no"),
			finding.height);
		if (finding.pin_bold || finding.pin_italic) {
			wxString fix;
			if (finding.pin_bold)
				fix += wxS("\\b1");
			if (finding.pin_italic)
				fix += wxS("\\i1");
			text += _("Suggested fix: ") + fix + wxS("\n");
		}
		else
			text += _("Suggested fix: Report only\n");
		text += _("Status: ") +
			(finding.safe_to_apply ? _("Safe to apply") : _("Review only")) + wxS("\n");
		text += _("Reason: ") + VariantReasonText(finding.reason_code) + wxS("\n");
		text += _("Rule: ") + to_wx(finding.reason_code);
	}

	details->SetValue(text);
	details->SetInsertionPoint(0);
}

void DialogFontPortabilityAudit::UpdateApplyButton() {
	apply_button->Enable(
		plan.names.catalog_available && plan.variants.catalog_available &&
		audit_selection.HasSelection());
	UpdateSummary();
}

void DialogFontPortabilityAudit::SelectSafe(bool select) {
	audit_selection.SelectSafe(plan, CurrentFilter(), select);
	for (unsigned int i = 0; i < findings->GetCount(); ++i) {
		auto const& visible = visible_findings[i];
		findings->Check(i, IsSelected(visible));
	}
	UpdateApplyButton();
}

void DialogFontPortabilityAudit::OnCheck(wxCommandEvent& event) {
	auto const index = event.GetInt();
	if (index < 0 || static_cast<std::size_t>(index) >= visible_findings.size())
		return;

	auto const& visible = visible_findings[index];
	auto const selected = IsSafe(visible) && findings->IsChecked(index);
	SetSelected(visible, selected);
	if (!selected)
		findings->Check(static_cast<unsigned int>(index), false);
	UpdateDetails();
	UpdateApplyButton();
}

void DialogFontPortabilityAudit::OnApply(wxCommandEvent&) {
	auto names = audit_selection.SelectedNameIndices();
	auto variants = audit_selection.SelectedVariantIndices();
	if (names.empty() && variants.empty())
		return;

	auto core = context->GetCore();
	auto const options = CurrentOptions();
	// A fresh resolver ensures Apply observes fonts installed or removed while
	// the dialog was open rather than reusing a scan-time request memo.
	auto resolver = CreatePlatformFontVariantResolver();
	auto live_profiles = MakeLiveProfileProvider(*resolver, CurrentCatalog());
	auto result = ApplyFontPortabilityAuditChanges(
		*core.ass, plan, names, variants, options, live_profiles);
	if (!result.success) {
		wxMessageBox(
			_("The subtitles or installed font profile changed after the audit. No fixes were applied.\n\n") +
				to_wx(result.error),
			_("Audit Font Portability"), wxOK | wxICON_ERROR | wxCENTER, this);
		BuildInitialPlan();
		return;
	}

	int commit_type = 0;
	if (result.styles_changed)
		commit_type |= AssFile::COMMIT_STYLES;
	if (result.dialogue_text_changed)
		commit_type |= AssFile::COMMIT_DIAG_TEXT;
	if (commit_type)
		core.ass->Commit(from_wx(_("apply font portability audit fixes")), commit_type);
	EndModal(wxID_OK);
}

template<FontPortabilityAuditFilter InitialFilter>
class FilteredFontPortabilityAuditDialog final : public DialogFontPortabilityAudit {
public:
	explicit FilteredFontPortabilityAuditDialog(agi::Context* c)
	: DialogFontPortabilityAudit(c, InitialFilter) {
	}
};

using DialogAllFontPortabilityFindings =
	FilteredFontPortabilityAuditDialog<FontPortabilityAuditFilter::All>;
using DialogFontNameFindings =
	FilteredFontPortabilityAuditDialog<FontPortabilityAuditFilter::Names>;
using DialogFontVariantFindings =
	FilteredFontPortabilityAuditDialog<FontPortabilityAuditFilter::Variants>;

} // namespace

void ShowFontPortabilityAuditDialog(agi::Context* c) {
	c->GetUI().dialog->ShowModal<DialogAllFontPortabilityFindings>(c);
}

void ShowFontNameNormalizationDialog(agi::Context* c) {
	c->GetUI().dialog->ShowModal<DialogFontNameFindings>(c);
}

void ShowFontVariantAuditDialog(agi::Context* c) {
	c->GetUI().dialog->ShowModal<DialogFontVariantFindings>(c);
}
