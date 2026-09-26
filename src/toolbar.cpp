// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file menutool.cpp
/// @brief Dynamic menu toolbar generator.
/// @ingroup toolbar menu

#include "automation/automation_debug_service.h"
#include "include/aegisub/toolbar.h"

#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/hotkey.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "retina_helper.h"
#include "utils.h"

#include <libaegisub/hotkey.h>
#include <libaegisub/json.h>
#include <libaegisub/log.h>
#include <libaegisub/signal.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <boost/interprocess/streams/bufferstream.hpp>
#include <vector>

#include <wx/button.h>
#include <wx/dcmemory.h>
#include <wx/frame.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/tglbtn.h>
#include <wx/toolbar.h>
#include <wx/wrapsizer.h>

namespace {
	constexpr size_t kMaxConfigurableToolbarItems = 64;

	json::Object const& get_root() {
		static json::Object root;
		if (root.empty()) {
			boost::interprocess::ibufferstream stream((const char *)default_toolbar, default_toolbar_size);
			root = std::move(static_cast<json::Object&>(agi::json_util::parse(stream)));
		}
		return root;
	}

	void add_toolbar_item(std::vector<std::string>& out, std::string item) {
		agi::util::strings::trim_inplace(item);
		if (item.empty() || item == "-") {
			if (!out.empty() && !out.back().empty())
				out.emplace_back();
			return;
		}

		out.emplace_back(std::move(item));
	}

	std::vector<std::string> parse_configurable_toolbar_items(std::string const& raw) {
		std::vector<std::string> items;
		agi::util::strings::for_each_split_any(raw, "\r\n", false, [&](agi::util::strings::view line) {
			agi::util::strings::for_each_split_any(line, ",;", false, [&](agi::util::strings::view item) {
				add_toolbar_item(items, std::string(item));
			});
		});

		while (!items.empty() && items.back().empty())
			items.pop_back();

		return items;
	}

	std::vector<std::string> normalize_configurable_toolbar_items(std::vector<std::string> const& raw) {
		std::vector<std::string> items;
		items.reserve(raw.size());
		for (auto const& item : raw)
			add_toolbar_item(items, item);

		while (!items.empty() && items.back().empty())
			items.pop_back();

		return items;
	}

	wxString strip_accelerators(wxString label) {
		label.Replace(wxS("&&"), wxS("\001"));
		label.Replace(wxS("&"), wxS(""));
		label.Replace(wxS("\001"), wxS("&"));
		label.Trim(true);
		label.Trim(false);
		return label;
	}

	bool is_badge_separator(wxUniChar ch) {
		auto const value = ch.GetValue();
		return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
			value == '/' || value == '\\' || value == '_' || value == '-' || value == '.';
	}

	wxString make_badge_text(wxString const& display, std::string const& command_name) {
		wxString label = strip_accelerators(display);
		if (label.empty())
			label = to_wx(command_name);

		wxString badge;
		for (size_t i = 0; i < label.length() && badge.length() < 2; ++i) {
			wxUniChar ch = label[i];
			if (is_badge_separator(ch))
				continue;
			wxString character;
			character += ch;
			badge += character.Upper();
		}

		return badge.empty() ? wxString(wxS("?")) : badge;
	}

	wxBitmap make_text_tool_bitmap(wxWindow *window, wxString const& display, std::string const& command_name, int icon_size, bool wide = false) {
		int const size = std::max(icon_size, window->FromDIP(18));
		int const width = wide ? std::max(size, window->FromDIP(100)) : size;
		int const inset = std::max(1, size / 8);
		wxBitmap bitmap(width, size);
		wxMemoryDC dc(bitmap);

		auto const face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
		dc.SetBackground(wxBrush(face));
		dc.Clear();

		dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW)));
		dc.SetBrush(wxBrush(face));
		dc.DrawRectangle(inset, inset, width - inset * 2, size - inset * 2);

		wxFont font = window->GetFont();
		if (font.IsOk()) {
			font.SetWeight(wxFONTWEIGHT_BOLD);
			dc.SetFont(font);
		}
		dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT));

		wxString label = wide ? strip_accelerators(display) : make_badge_text(display, command_name);
		if (label.empty() && !wide)
			label = to_wx(command_name);

		wxSize text_size = dc.GetTextExtent(label);
		if (font.IsOk()) {
			int point_size = font.GetPointSize();
			if (wide) {
				while (point_size > 6 && text_size.x > width - inset * 4) {
					font.SetPointSize(--point_size);
					dc.SetFont(font);
					text_size = dc.GetTextExtent(label);
				}
			}
			else {
				while (point_size > 6 && (text_size.x > size - inset * 2 || text_size.y > size - inset * 2)) {
					font.SetPointSize(--point_size);
					dc.SetFont(font);
					text_size = dc.GetTextExtent(label);
				}
			}
		}

		dc.DrawText(label, (width - text_size.x) / 2, (size - text_size.y) / 2);
		dc.SelectObject(wxNullBitmap);
		return bitmap;
	}

	class Toolbar final : public wxToolBar {
		/// Window ID of first toolbar control
		static const int TOOL_ID_BASE = 5000;

		/// Toolbar name in config file
		std::string name;
		/// Option containing a user-editable command list, if this toolbar is configurable
		std::string command_option;
		/// Project context
		agi::Context *context;
		/// Commands for each of the buttons
		std::vector<cmd::Command *> commands;
		/// Hotkey context
		std::string ht_context;

		RetinaHelper retina_helper;

		/// Current icon size
		int icon_size;

		/// Listener for icon size change signal
		agi::signal::Connection icon_size_slot;

		agi::signal::Connection command_list_slot;

		/// Listener for hotkey change signal
		agi::signal::Connection hotkeys_changed_slot;
		agi::signal::Connection video_dpi_slot;

		bool UsesVideoUiToolbarIcons() const {
			return name == "video" || name == "visual_tools";
		}

		int GetVideoToolbarIconSize() const {
			return GetVideoUiIconSize(
				const_cast<Toolbar *>(this),
				OPT_GET("App/Toolbar Icon Size")->GetInt());
		}

		/// Enable/disable the toolbar buttons
		void OnIdle(wxIdleEvent &) {
			if (context->lua_workspace_invocation_active || (config::automation_debug_service && config::automation_debug_service->HasLocalSession()))
				return;
			for (size_t i = 0; i < commands.size(); ++i) {
				int const id = TOOL_ID_BASE + static_cast<int>(i);
				if (commands[i]->Type() & cmd::COMMAND_VALIDATE) {
					bool enabled = commands[i]->Validate(context);
					if (GetToolEnabled(id) != enabled)
						EnableTool(id, enabled);
				}
				if (commands[i]->Type() & cmd::COMMAND_TOGGLE || commands[i]->Type() & cmd::COMMAND_RADIO) {
					bool active = commands[i]->IsActive(context);
					if (GetToolState(id) != active)
						ToggleTool(id, active);
				}
			}
		}

		/// Toolbar button click handler
		void OnClick(wxCommandEvent &evt) {
			if (context->lua_workspace_invocation_active || (config::automation_debug_service && config::automation_debug_service->HasLocalSession()))
				return;
			auto *cmd = commands[evt.GetId() - TOOL_ID_BASE];
			if (cmd->Type() & cmd::COMMAND_VALIDATE && !cmd->Validate(context))
				return;
			(*cmd)(context);
		}

		/// Regenerate the toolbar when the icon size changes
		void OnIconSizeChange(agi::OptionValue const& opt) {
			icon_size = opt.GetInt();
			RegenerateToolbar();
		}

		void OnCommandListChange(agi::OptionValue const&) {
			RegenerateToolbar();
		}

		/// Clear the toolbar and recreate it
		void RegenerateToolbar() {
			Unbind(wxEVT_IDLE, &Toolbar::OnIdle, this);
			ClearTools();
			commands.clear();
			Populate();
		}

		std::vector<std::string> GetConfiguredCommands() const {
			if (!command_option.empty()) {
				auto opt = OPT_GET(command_option);
				if (opt->GetType() == agi::OptionType::ListString)
					return normalize_configurable_toolbar_items(opt->GetListString());
				return parse_configurable_toolbar_items(opt->GetString());
			}

			json::Object const& root = get_root();
			auto root_it = root.find(name);
			if (root_it == root.end()) {
				// Toolbar names are all hardcoded so this should never happen
				throw agi::InternalError("Toolbar named " + name + " not found.");
			}

			json::Array const& arr = root_it->second;
			std::vector<std::string> command_names;
			command_names.reserve(arr.size());
			for (json::String const& command_name : arr)
				command_names.emplace_back(command_name);

			return command_names;
		}

		/// Populate the toolbar with buttons
		void Populate() {
			auto command_names = GetConfiguredCommands();
			commands.reserve(command_names.size());
			bool needs_onidle = false;
			bool last_was_sep = false;
			if (UsesVideoUiToolbarIcons()) {
				int const tool_icon_size = GetVideoToolbarIconSize();
				SetToolBitmapSize(wxSize(tool_icon_size, tool_icon_size));
			}

			size_t item_count = 0;
			for (std::string const& raw_name : command_names) {
				if (!command_option.empty() && item_count++ >= kMaxConfigurableToolbarItems) {
					LOG_W("toolbar/configurable/too_many_items") << "Toolbar '" << name << "' has more than "
						<< kMaxConfigurableToolbarItems << " configured items; ignoring the rest";
					break;
				}

				// Parse "command_name|Display Name" format for configurable toolbars
				auto [cmd_name, custom_display] = command_option.empty()
					? std::pair(raw_name, std::string())
					: toolbar::ParseCommandEntry(raw_name);

				if (cmd_name.empty()) {
					if (!last_was_sep)
						AddSeparator();
					last_was_sep = true;
					continue;
				}

				auto *command = cmd::get_if(cmd_name);
				if (!command) {
					LOG_D("toolbar/command/not_found") << "Command '" << cmd_name << "' not found; skipping";
					continue;
				}

				last_was_sep = false;

				wxString const display = !custom_display.empty()
					? to_wx(custom_display)
					: command->StrDisplay(context);

				int flags = command->Type();
				wxItemKind kind =
					flags & cmd::COMMAND_RADIO ? wxITEM_RADIO :
					flags & cmd::COMMAND_TOGGLE ? wxITEM_CHECK :
					wxITEM_NORMAL;

				auto const layout_direction = GetLayoutDirection();
				if (UsesVideoUiToolbarIcons()) {
					int const tool_icon_size = GetVideoToolbarIconSize();
					wxBitmap bitmap = command->Icon(tool_icon_size, layout_direction);
					if (!bitmap.IsOk())
						bitmap = make_text_tool_bitmap(this, display, cmd_name, tool_icon_size, !command_option.empty());
					AddTool(TOOL_ID_BASE + commands.size(), display, bitmap, GetTooltip(command), kind);
				}
				else {
					// Ask for the bundle directly: probing with Icon() first would
					// decode and rescale a bitmap that is then thrown away, and
					// both accessors come from the same CMD_ICON macro, so an
					// empty bundle already means "this command has no icon".
					wxBitmapBundle bundle = command->IconBundle(layout_direction);
					if (!bundle.IsOk())
						bundle = wxBitmapBundle::FromBitmap(make_text_tool_bitmap(this, display, cmd_name, icon_size, !command_option.empty()));
					AddTool(TOOL_ID_BASE + commands.size(), display, bundle, GetTooltip(command), kind);
				}

				commands.push_back(command);
				needs_onidle = needs_onidle || flags != cmd::COMMAND_NORMAL;
			}

			// Only bind the update function if there are actually any dynamic tools
			if (needs_onidle) {
				Bind(wxEVT_IDLE, &Toolbar::OnIdle, this);
			}

			Realize();
		}

		wxString GetTooltip(cmd::Command *command) {
			wxString ret = command->StrHelp();

			std::vector<std::string> hotkeys = hotkey::get_hotkey_strs(ht_context, command->name());
			if (!hotkeys.empty())
				ret += to_wx(" (" + agi::util::strings::join(hotkeys, "/") + ")");

			return ret;
		}

	public:
		Toolbar(wxWindow *parent, std::string name, std::string command_option, agi::Context *c, std::string ht_context, bool vertical)
		: wxToolBar(parent, -1, wxDefaultPosition, wxDefaultSize, wxTB_NODIVIDER | wxTB_FLAT | (vertical ? wxTB_VERTICAL : wxTB_HORIZONTAL))
		, name(std::move(name))
		, command_option(std::move(command_option))
		, context(c)
		, ht_context(std::move(ht_context))
		, retina_helper(parent)
#ifdef __WXMSW__
		, icon_size(AEGI_BITMAP_ICON_SIZE(parent, 16))
#else
		, icon_size(OPT_GET("App/Toolbar Icon Size")->GetInt())
#endif
		, icon_size_slot(OPT_SUB("App/Toolbar Icon Size", &Toolbar::OnIconSizeChange, this))
		, hotkeys_changed_slot(hotkey::inst->AddHotkeyChangeListener(&Toolbar::RegenerateToolbar, this))
		, video_dpi_slot(UsesVideoUiToolbarIcons() ? OPT_SUB("Video/Scale with DPI", [=](agi::OptionValue const&) { RegenerateToolbar(); }) : agi::signal::Connection())
		{
			if (!this->command_option.empty())
				command_list_slot = OPT_SUB(this->command_option, &Toolbar::OnCommandListChange, this);
			Populate();
			Bind(wxEVT_TOOL, &Toolbar::OnClick, this);
		}

		Toolbar(wxFrame *parent, std::string name, agi::Context *c, std::string ht_context)
		: wxToolBar(parent, -1, wxDefaultPosition, wxDefaultSize, wxTB_FLAT | wxTB_HORIZONTAL)
		, name(std::move(name))
		, context(c)
		, ht_context(std::move(ht_context))
		, retina_helper(parent)
#ifndef __WXMAC__
#ifdef __WXMSW__
		, icon_size(AEGI_BITMAP_ICON_SIZE(parent, 16))
#else
		, icon_size(OPT_GET("App/Toolbar Icon Size")->GetInt())
#endif
		, icon_size_slot(OPT_SUB("App/Toolbar Icon Size", &Toolbar::OnIconSizeChange, this))
#else
		, icon_size(32 * retina_helper.GetScaleFactor())
		, icon_size_slot(retina_helper.AddScaleFactorListener([=](double scale) {
			icon_size = 32 * retina_helper.GetScaleFactor();
			RegenerateToolbar();
		}))
#endif
		, hotkeys_changed_slot(hotkey::inst->AddHotkeyChangeListener(&Toolbar::RegenerateToolbar, this))
		, video_dpi_slot(UsesVideoUiToolbarIcons() ? OPT_SUB("Video/Scale with DPI", [=](agi::OptionValue const&) { RegenerateToolbar(); }) : agi::signal::Connection())
		{
			parent->SetToolBar(this);
			Populate();
			Bind(wxEVT_TOOL, &Toolbar::OnClick, this);
		}
	};

	class WrappingCommandPanel : public wxPanel {
		static const int CMD_ID_BASE = 6000;

		std::string command_option;
		agi::Context *context;
		std::vector<cmd::Command *> commands;
		std::vector<wxWindow *> buttons;
		std::string ht_context;

		agi::signal::Connection command_list_slot;

		void Regenerate() {
			Unbind(wxEVT_IDLE, &WrappingCommandPanel::OnIdle, this);
			DestroyChildren();
			commands.clear();
			buttons.clear();
			Populate();
			InvalidateBestSize();
		}

		void OnIdle(wxIdleEvent &) {
			for (size_t i = 0; i < commands.size(); ++i) {
				auto *btn = buttons[i];
				if (context->lua_workspace_invocation_active || (config::automation_debug_service && config::automation_debug_service->HasLocalSession()))
					return;
				if (!btn) continue;

				if (commands[i]->Type() & cmd::COMMAND_VALIDATE) {
					bool enabled = commands[i]->Validate(context);
					if (btn->IsThisEnabled() != enabled)
						btn->Enable(enabled);
				}
				if (commands[i]->Type() & cmd::COMMAND_TOGGLE) {
					auto *tgl = dynamic_cast<wxToggleButton *>(btn);
					if (tgl) {
						bool active = commands[i]->IsActive(context);
						if (tgl->GetValue() != active)
							tgl->SetValue(active);
					}
				}
				// COMMAND_RADIO is not supported in the wrapping panel;
				// radio button groups require mutual exclusion logic
				// that is not available with individual wxButtons.
			}
		}

		void OnCommandEvent(wxCommandEvent &evt) {
			if (context->lua_workspace_invocation_active || (config::automation_debug_service && config::automation_debug_service->HasLocalSession()))
				return;
			auto idx = evt.GetId() - CMD_ID_BASE;
			if (idx < 0 || static_cast<size_t>(idx) >= commands.size())
				return;
			auto *cmd = commands[idx];
			if (cmd->Type() & cmd::COMMAND_VALIDATE && !cmd->Validate(context))
				return;
			(*cmd)(context);
		}

		void Populate() {
			auto opt = OPT_GET(command_option);
			std::vector<std::string> command_names;
			if (opt->GetType() == agi::OptionType::ListString)
				command_names = normalize_configurable_toolbar_items(opt->GetListString());
			else
				command_names = parse_configurable_toolbar_items(opt->GetString());

			commands.reserve(command_names.size());
			bool needs_onidle = false;

			auto *sizer = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
			size_t item_count = 0;
			bool last_was_sep = false;
			for (std::string const& raw_name : command_names) {
				if (item_count++ >= kMaxConfigurableToolbarItems)
					break;

				auto [cmd_name, custom_display] = toolbar::ParseCommandEntry(raw_name);

				if (cmd_name.empty()) {
					last_was_sep = true;
					continue;
				}

				if (last_was_sep) {
					auto *sep = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(2), -1), wxLI_VERTICAL);
					sizer->Add(sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));
					last_was_sep = false;
				}

				auto *command = cmd::get_if(cmd_name);
				if (!command) continue;

				wxString display;
				if (!custom_display.empty()) {
					display = to_wx(custom_display);
				}
				else {
					display = strip_accelerators(command->StrDisplay(context));
					if (display.empty())
						display = to_wx(cmd_name);
				}
				wxWindow *btn;
				if (command->Type() & cmd::COMMAND_TOGGLE)
					btn = new wxToggleButton(this, CMD_ID_BASE + static_cast<int>(commands.size()), display,
						wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
				else
					// COMMAND_RADIO commands are treated as regular wxButton;
					// radio group mutual exclusion is not implemented here
					btn = new wxButton(this, CMD_ID_BASE + static_cast<int>(commands.size()), display,
						wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
				btn->SetToolTip(GetTooltip(command));
				sizer->Add(btn, 0, wxALL, FromDIP(1));
				commands.push_back(command);
				buttons.push_back(btn);
				needs_onidle = needs_onidle || command->Type() != cmd::COMMAND_NORMAL;
			}

			auto *outer_sizer = new wxBoxSizer(wxVERTICAL);
			outer_sizer->Add(sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(2));
			SetSizer(outer_sizer);
			Layout();
			if (needs_onidle)
				Bind(wxEVT_IDLE, &WrappingCommandPanel::OnIdle, this);
		}

		wxString GetTooltip(cmd::Command *command) {
			wxString ret = command->StrHelp();
			std::vector<std::string> hotkeys = hotkey::get_hotkey_strs(ht_context, command->name());
			if (!hotkeys.empty())
				ret += to_wx(" (" + agi::util::strings::join(hotkeys, "/") + ")");
			return ret;
		}

	public:
		WrappingCommandPanel(wxWindow *parent, std::string const& cmd_opt, agi::Context *c, std::string const& hk_ctx)
		: wxPanel(parent, -1)
		, command_option(cmd_opt)
		, context(c)
		, ht_context(hk_ctx)
		, command_list_slot(OPT_SUB(command_option, [this](agi::OptionValue const&) { Regenerate(); })) {
			Populate();
			Bind(wxEVT_BUTTON, &WrappingCommandPanel::OnCommandEvent, this);
			Bind(wxEVT_TOGGLEBUTTON, &WrappingCommandPanel::OnCommandEvent, this);
		}

	protected:
		// Override DoGetBestSize to cap height to one row of buttons,
		// preventing wxWrapSizer from stacking vertically at 0 width
		wxSize DoGetBestSize() const override {
			wxSize best = wxPanel::DoGetBestSize();
			wxSize btn_size = wxButton::GetDefaultSize();
			int single_row_height = btn_size.GetHeight() + FromDIP(6);
			if (best.GetHeight() > single_row_height)
				best.SetHeight(single_row_height);
			return best;
		}
	};
}

namespace toolbar {
	void AttachToolbar(wxFrame *frame, std::string const& name, agi::Context *c, std::string const& hotkey) {
		new Toolbar(frame, name, c, hotkey);
	}

	wxToolBar *GetToolbar(wxWindow *parent, std::string const& name, agi::Context *c, std::string const& hotkey, bool vertical) {
		return new Toolbar(parent, name, std::string(), c, hotkey, vertical);
	}

	wxToolBar *GetOptionToolbar(wxWindow *parent, std::string const& name, std::string const& command_option, agi::Context *c, std::string const& hotkey, bool vertical) {
		return new Toolbar(parent, name, command_option, c, hotkey, vertical);
	}

	wxPanel *GetOptionToolbarWrapping(wxWindow *parent, std::string const& command_option, agi::Context *c, std::string const& hotkey) {
		return new WrappingCommandPanel(parent, command_option, c, hotkey);
	}
}
