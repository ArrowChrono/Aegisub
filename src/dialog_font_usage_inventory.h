#pragma once

#include <memory>

#include <wx/dialog.h>

namespace agi {
struct Context;
}

class DialogFontUsageInventory final : public wxDialog {
	struct Impl;
	std::unique_ptr<Impl> impl;

	public:
	explicit DialogFontUsageInventory(agi::Context *context);
	~DialogFontUsageInventory() override;

	void NotifySystemFontsChanged();
};

void ShowFontUsageInventoryDialog(agi::Context *context);
void NotifyFontUsageInventorySystemFontsChanged(agi::Context *context);
