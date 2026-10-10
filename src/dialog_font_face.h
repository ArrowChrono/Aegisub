#pragma once

#include <functional>
#include <cstdint>
#include <optional>
#include <string>

struct FontFamilySelectionModel;
class wxWindow;

struct FontFaceDialogSelection {
	std::string face_name;
	/// Catalog identity for an explicitly selected list item. This disambiguates
	/// duplicate display aliases without serializing an implementation token.
	std::optional<std::uint32_t> selected_family_id;
	int point_size = 10;
	int charset = 1;
	/// Effective ASS/GDI weight. Values other than 400/700 are retained until
	/// the user explicitly changes the variant controls.
	int effective_weight = 400;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool has_explicit_weight = false;
	bool has_explicit_italic = false;
	bool face_modified = false;
	bool variant_modified = false;
	bool weight_modified = false;
	bool italic_modified = false;
	bool implicit_variant_pinned = false;
	bool allow_replace_explicit = false;
	bool from_native_dialog = false;

};

/// Callbacks that let the dialog drive a live, revertible edit of the document
/// so the video frame shows the real result while the dialog is open.
///
/// Lifecycle: zero or more `on_preview` calls, then either `on_commit` (OK) or
/// `on_revert` (Cancel/close).
struct FontFaceDialogHooks {
	/// Apply the selection to the document as a temporary preview. Calls must
	/// not alter undo/redo or persistent modified state, and must be idempotent:
	/// each call fully replaces the previous preview rather than stacking.
	///
	/// The dialog debounces control changes before calling this, so an
	/// implementation may commit and re-render without extra throttling.
	std::function<void(FontFaceDialogSelection const&)> on_preview;

	/// Make the current preview permanent (OK). Any deferred preview
	/// is flushed first; this is where the caller creates the single undo entry.
	std::function<void(FontFaceDialogSelection const&)> on_commit;

	/// Discard everything previewed since the last `on_commit`. Only called
	/// when at least one `on_preview` was emitted.
	std::function<void()> on_revert;
};

/// Show Aegisub's custom selector using localized or English catalog names as
/// configured. If the catalog is empty, the selector uses enumerated fallback
/// names. The displayed face name is also the exact face name returned for ASS
/// writing.
///
/// Returns the selection that was made permanent, or nullopt if the dialog was
/// cancelled. On OK the hooks have already committed it, so callers must not
/// apply the returned selection a second time.
std::optional<FontFaceDialogSelection> ShowFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilySelectionModel const& font_model,
	FontFaceDialogHooks hooks = {});
