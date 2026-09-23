#include "options.h"
#include "subs_edit_ctrl_stc.h"
#include "command/command.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#include <boost/locale/generator.hpp>

#include <wx/app.h>
#include <wx/frame.h>
#include <wx/init.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class SubsEditSmokeApp final : public wxApp {
	public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(SubsEditSmokeApp);

// Null-context SetTextTo must never invoke application commands. Keep this
// boundary fail-fast instead of hosting the unrelated application controllers.
namespace cmd {
void call(std::string const&, agi::Context *) {
	throw std::runtime_error("unexpected application command in null-context STC smoke");
}

Command *get(std::string const&) {
	throw std::runtime_error("unexpected command lookup in null-context STC smoke");
}
}

namespace {
void Require(bool condition, std::string const& message) {
	if (!condition)
		throw std::runtime_error(message);
}

std::string Text(SubsStyledTextEditCtrl& control) {
	auto const text = control.GetTextRaw();
	return {text.data(), text.length()};
}

class Runtime final {
	agi::log::LogSink log;
	agi::Path paths;
	std::unique_ptr<agi::Options> options;

	public:
	Runtime() {
		agi::log::log = &log;
		std::ifstream input("src/libresrc/default_config.json", std::ios::binary);
		Require(input.good(), "run the smoke test from the repository root");
		std::string defaults{std::istreambuf_iterator<char>(input), {}};
		options = std::make_unique<agi::Options>(
			std::filesystem::path{}, std::pair{defaults.data(), defaults.size()}, agi::Options::FLUSH_SKIP);
		config::opt = options.get();
		config::path = &paths;
		paths.SetToken("?data", std::filesystem::current_path());
		paths.SetToken("?user", std::filesystem::temp_directory_path());
		OPT_SET("Tool/Thesaurus/Language")->SetString("");
		OPT_SET("App/Call Tips")->SetBool(false);
		OPT_SET("Subtitle/Edit Box/Use DirectWrite")->SetBool(false);
		OPT_SET("Subtitle/Highlight/Syntax")->SetBool(true);
		OPT_SET("Subtitle/Highlight/Color Swatches")->SetBool(true);
		OPT_SET("Subtitle/Edit Box/Character Markers/Show/Space")->SetBool(true);
		OPT_SET("Subtitle/Edit Box/Character Markers/Error/Enabled")->SetBool(false);
		agi::dispatch::Init([](agi::dispatch::Thunk const& thunk) { wxTheApp->CallAfter(thunk); });
	}

	~Runtime() {
		agi::dispatch::Shutdown();
		config::path = nullptr;
		config::opt = nullptr;
		options.reset();
		agi::log::log = nullptr;
	}
};

class Fixture final {
	std::unique_ptr<wxFrame> frame;

	public:
	struct PaintedSnapshot {
		std::string text;
		std::vector<int> styles;
	};

	SubsStyledTextEditCtrl *control = nullptr;
	int text_modifications = 0;
	int paints = 0;
	std::vector<PaintedSnapshot> painted_snapshots;

	Fixture()
		: frame(std::make_unique<wxFrame>(nullptr, wxID_ANY, wxS("Subtitle edit smoke"),
										  wxPoint(-32000, -32000), wxSize(600, 200), wxFRAME_NO_TASKBAR)) {
		control = new SubsStyledTextEditCtrl(frame.get(), wxSize(580, 160), 0, nullptr);
		control->Bind(wxEVT_STC_MODIFIED, [this](wxStyledTextEvent& event) {
			if (event.GetModificationType() & (wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT))
				++text_modifications;
			event.Skip();
		});
		control->Bind(wxEVT_STC_PAINTED, [this](wxStyledTextEvent& event) {
			++paints;
			PaintedSnapshot snapshot{.text = Text(*control), .styles = {}};
			for (int byte = 0; byte < control->GetTextLength(); ++byte)
				snapshot.styles.push_back(control->GetStyleAt(byte));
			painted_snapshots.push_back(std::move(snapshot));
			event.Skip();
		});
		frame->ShowWithoutActivating();
		// Initial window setup is the only explicit invalidation in this fixture.
		control->Refresh(false);
		DrainPaintWithoutRefresh();
	}

	void DrainPaintWithoutRefresh() const {
		control->Update();
		wxTheApp->ProcessPendingEvents();
	}
};

void RequireCaret(SubsStyledTextEditCtrl const& control, int expected) {
	Require(control.GetCurrentPos() == expected, "caret byte offset is not the old character index");
	Require(control.GetSelectionStart() == expected && control.GetSelectionEnd() == expected,
			"changed text must collapse selection at the restored caret");
}

void Utf8CaretAndNoopSelection() {
	Fixture fixture;
	auto& control = *fixture.control;
	std::string const first = "A\xE4\xB8\xAD\xF0\x9F\x98\x80"
							  "B";
	control.SetTextTo(first);
	control.SetSelection(1, 8); // Caret after A, CJK, and the four-byte emoji: character index 3.
	std::string const longer = "XY\xE4\xB8\xAD\xF0\x9F\x98\x80"
							   "B";
	control.SetTextTo(longer);
	Require(Text(control) == longer, "UTF-8 prefix insertion corrupted the control text");
	RequireCaret(control, 5); // Same character index, now before the emoji.

	control.SetSelection(0, 9); // Four characters before B.
	std::string const shorter = "\xE4\xB8\xAD\xF0\x9F\x98\x80";
	control.SetTextTo(shorter);
	Require(Text(control) == shorter, "UTF-8 shortening corrupted the control text");
	RequireCaret(control, 7); // Out-of-range old character index clamps to end.

	control.SetSelection(0, 3);
	control.SetTextTo(shorter);
	Require(control.GetSelectionStart() == 0 && control.GetSelectionEnd() == 3,
			"identical cached text must leave a nonempty selection unchanged");
	control.SetTextTo("");
	Require(Text(control).empty(), "deleting all text did not empty the control");
	RequireCaret(control, 0);
	Require(fixture.text_modifications == 0, "SetTextTo emitted a text-edit notification");
}

struct StyledChunk {
	std::string_view text;
	int style;
};

void RequireStyles(SubsStyledTextEditCtrl& control, std::initializer_list<StyledChunk> chunks) {
	int position = 0;
	std::string expected;
	for (auto const& chunk : chunks) {
		expected += chunk.text;
		for (std::size_t byte = 0; byte < chunk.text.size(); ++byte, ++position)
			Require(control.GetStyleAt(position) == chunk.style,
					"incorrect syntax style at byte " + std::to_string(position));
	}
	Require(Text(control) == expected, "style oracle does not describe the exact final text");
	Require(control.GetEndStyled() == control.GetTextLength(), "styling did not reach the final byte");
}

void RequireNaturalPaints(Fixture const& fixture, int previous_paints,
						  std::initializer_list<StyledChunk> chunks) {
	Require(fixture.paints > previous_paints, "synchronized text did not naturally invalidate and paint");
	std::string expected_text;
	std::vector<int> expected_styles;
	for (auto const& chunk : chunks) {
		expected_text += chunk.text;
		expected_styles.insert(expected_styles.end(), chunk.text.size(), chunk.style);
	}
	Require(!fixture.painted_snapshots.empty(), "native paint produced no captured snapshot");
	for (auto const& snapshot : fixture.painted_snapshots) {
		Require(snapshot.text == expected_text, "native paint observed stale or partial synchronized text");
		Require(snapshot.styles == expected_styles, "native paint observed stale or partial syntax styles");
	}
}

void RequireIndicators(SubsStyledTextEditCtrl& control, int swatch_begin, int swatch_end,
					   int swatch_color, int space) {
	for (int byte = 0; byte < control.GetTextLength(); ++byte) {
		int const expected_swatch = byte >= swatch_begin && byte < swatch_end
										? wxSTC_INDICVALUEBIT | swatch_color
										: 0;
		Require(control.IndicatorValueAt(10, byte) == expected_swatch,
				"stale or incorrect color swatch at byte " + std::to_string(byte));
		int const space_indicators = control.IndicatorAllOnFor(byte) & ((1 << 4) | (1 << 7));
		Require(space_indicators == (byte == space ? 1 << 4 : 0),
				"stale or incorrect space marker at byte " + std::to_string(byte));
	}
}

void NumericAndStructuralStyles() {
	namespace style = agi::ass::SyntaxStyle;
	Fixture fixture;
	auto& control = *fixture.control;
	control.SetTextTo("{\\pos(1,2)\\c&H0000FF&}A B");
	RequireStyles(control, {{.text = "{", .style = style::OVERRIDE}, {.text = "\\", .style = style::PUNCTUATION}, {.text = "pos", .style = style::TAG}, {.text = "(", .style = style::PUNCTUATION}, {.text = "1", .style = style::PARAMETER}, {.text = ",", .style = style::PUNCTUATION}, {.text = "2", .style = style::PARAMETER}, {.text = ")\\", .style = style::PUNCTUATION}, {.text = "c", .style = style::TAG}, {.text = "&H0000FF&", .style = style::PARAMETER}, {.text = "}", .style = style::OVERRIDE}, {.text = "A B", .style = style::NORMAL}});
	RequireIndicators(control, 14, 20, 0x0000FF, 23);

	control.SetTextTo("{\\pos(120,-24)\\c&H00FF00&}A B");
	std::initializer_list<StyledChunk> const numeric_styles = {{.text = "{", .style = style::OVERRIDE}, {.text = "\\", .style = style::PUNCTUATION}, {.text = "pos", .style = style::TAG}, {.text = "(", .style = style::PUNCTUATION}, {.text = "120", .style = style::PARAMETER}, {.text = ",", .style = style::PUNCTUATION}, {.text = "-24", .style = style::PARAMETER}, {.text = ")\\", .style = style::PUNCTUATION}, {.text = "c", .style = style::TAG}, {.text = "&H00FF00&", .style = style::PARAMETER}, {.text = "}", .style = style::OVERRIDE}, {.text = "A B", .style = style::NORMAL}};
	RequireStyles(control, numeric_styles);
	RequireIndicators(control, 18, 24, 0x00FF00, 27);

	OPT_SET("Subtitle/Highlight/Syntax")->SetBool(false);
	RequireStyles(control, {{.text = "{\\pos(120,-24)\\c&H00FF00&}A B", .style = style::NORMAL}});
	RequireIndicators(control, 18, 24, 0x00FF00, 27);
	OPT_SET("Subtitle/Highlight/Syntax")->SetBool(true);
	RequireStyles(control, numeric_styles);
	RequireIndicators(control, 18, 24, 0x00FF00, 27);

	control.SetTextTo("{\\b1}A\\NB");
	RequireStyles(control, {{.text = "{", .style = style::OVERRIDE}, {.text = "\\", .style = style::PUNCTUATION}, {.text = "b", .style = style::TAG}, {.text = "1", .style = style::PARAMETER}, {.text = "}", .style = style::OVERRIDE}, {.text = "A", .style = style::NORMAL}, {.text = "\\N", .style = style::LINE_BREAK}, {.text = "B", .style = style::NORMAL}});
	RequireIndicators(control, 0, 0, 0, -1);
	control.SetTextTo("A B");
	RequireStyles(control, {{.text = "A B", .style = style::NORMAL}});
	RequireIndicators(control, 0, 0, 0, 1);
	Require(fixture.text_modifications == 0, "styling exposed a text-edit notification");
}

void EventAndUndoState() {
	Fixture fixture;
	auto& control = *fixture.control;
	for (bool const handlers_enabled : {false, true}) {
		for (bool const collect_undo : {false, true}) {
			control.SetEvtHandlerEnabled(handlers_enabled);
			control.SetUndoCollection(collect_undo);
			control.EmptyUndoBuffer();
			int const mask = wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT;
			control.SetModEventMask(mask);
			control.SetTextTo(handlers_enabled ? "{\\pos(20,3)}visible" : "{\\pos(2,3)}disabled");
			Require(control.GetEvtHandlerEnabled() == handlers_enabled, "event-handler state changed");
			Require(control.GetUndoCollection() == collect_undo, "undo-collection mode changed");
			Require(control.GetModEventMask() == mask, "modification event mask changed");
			Require(!control.CanUndo() && !control.CanRedo(), "external synchronization entered native undo history");
			// Ensure both iterations take the changed-text path.
			control.SetTextTo("reset");
		}
	}
	Require(fixture.text_modifications == 0, "external synchronization emitted insert/delete events");
	control.SetEvtHandlerEnabled(true);
	control.SetUndoCollection(true);
	control.SetTextTo("final");
	control.SetSelection(5, 5);
	control.AddTextRaw("!");
	Require(Text(control) == "final!" && control.CanUndo(), "native editing was not restored");
	Require(fixture.text_modifications > 0, "native editing notifications were not restored");
	control.Undo();
	Require(Text(control) == "final", "native undo did not restore the synchronized text");
}

void OuterFreezeAndFinalPaint() {
	namespace style = agi::ass::SyntaxStyle;
	Fixture fixture;
	auto& control = *fixture.control;
	control.SetTextTo("first");
	fixture.DrainPaintWithoutRefresh();
	fixture.painted_snapshots.clear();
	int const before = fixture.paints;
	control.Freeze();
	control.Freeze();
	control.SetTextTo("{\\b1}final");
	Require(control.IsFrozen(), "SetTextTo released the caller's freeze");
	control.Thaw();
	Require(control.IsFrozen(), "SetTextTo changed the outer nested freeze count");
	control.Thaw();
	Require(!control.IsFrozen(), "SetTextTo left an extra freeze outstanding");
	fixture.DrainPaintWithoutRefresh();
	RequireNaturalPaints(fixture, before, {{.text = "{", .style = style::OVERRIDE}, {.text = "\\", .style = style::PUNCTUATION}, {.text = "b", .style = style::TAG}, {.text = "1", .style = style::PARAMETER}, {.text = "}", .style = style::OVERRIDE}, {.text = "final", .style = style::NORMAL}});
}

void UnfrozenNaturalPaint() {
	namespace style = agi::ass::SyntaxStyle;
	Fixture fixture;
	auto& control = *fixture.control;
	control.SetTextTo("{\\b1}first");
	fixture.DrainPaintWithoutRefresh();
	// Prime the unchanged-text fast path before another real synchronization.
	control.SetTextTo("{\\b1}first");
	fixture.DrainPaintWithoutRefresh();
	fixture.painted_snapshots.clear();
	int const before = fixture.paints;
	control.SetTextTo("{\\b0}second");
	fixture.DrainPaintWithoutRefresh();
	RequireNaturalPaints(fixture, before, {{.text = "{", .style = style::OVERRIDE}, {.text = "\\", .style = style::PUNCTUATION}, {.text = "b", .style = style::TAG}, {.text = "0", .style = style::PARAMETER}, {.text = "}", .style = style::OVERRIDE}, {.text = "second", .style = style::NORMAL}});
	Require(!control.IsFrozen(), "unfrozen synchronization left redraw frozen");
}

void WrapAndScrollbarThresholds() {
	Fixture fixture;
	auto& control = *fixture.control;
	control.SetTextTo("short");
	fixture.DrainPaintWithoutRefresh();
	Require(control.WrapCount(0) == 1, "short text unexpectedly wraps");
	std::string long_text;
	for (int word = 0; word < 600; ++word)
		long_text += "wrapped word ";
	control.SetTextTo(long_text);
	fixture.DrainPaintWithoutRefresh();
	Require(Text(control) == long_text, "wrapping changed synchronized text");
	Require(control.WrapCount(0) > control.LinesOnScreen(),
			"long text did not cross the visible wrapped-line threshold");
	control.ScrollToEnd();
	fixture.DrainPaintWithoutRefresh();
	Require(control.GetFirstVisibleLine() > 0, "wrapped text did not become scrollable");
	control.SetTextTo("short again");
	fixture.DrainPaintWithoutRefresh();
	Require(Text(control) == "short again", "shrinking scrollable text left stale content");
	Require(control.WrapCount(0) == 1, "shrinking text did not recompute wrapping");
	Require(control.GetFirstVisibleLine() == 0, "shrinking text left the viewport scrolled past the text");
	Require(!control.IsFrozen(), "wrap/scrollbar transitions left redraw frozen");
	Require(fixture.text_modifications == 0, "wrap/scrollbar transitions emitted editing events");
}
}

int main(int argc, char **argv) {
	if (!wxEntryStart(argc, argv)) {
		std::cerr << "wxWidgets initialization failed\n";
		return 1;
	}
	int result = 0;
	try {
		Require(wxTheApp->CallOnInit(), "wxApp initialization failed");
		std::locale::global(boost::locale::generator().generate(""));
		for (auto const& test : {std::pair{"Utf8CaretAndNoopSelection", Utf8CaretAndNoopSelection},
								 std::pair{"NumericAndStructuralStyles", NumericAndStructuralStyles},
								 std::pair{"EventAndUndoState", EventAndUndoState},
								 std::pair{"OuterFreezeAndFinalPaint", OuterFreezeAndFinalPaint},
								 std::pair{"UnfrozenNaturalPaint", UnfrozenNaturalPaint},
								 std::pair{"WrapAndScrollbarThresholds", WrapAndScrollbarThresholds}}) {
			// The control's unscoped option listeners must not survive into another case.
			Runtime runtime;
			std::cout << "RUN " << test.first << '\n';
			test.second();
			std::cout << "PASS " << test.first << '\n';
		}
	}
	catch (agi::Exception const& error) {
		std::cerr << "FAIL " << error.GetMessage() << '\n';
		result = 1;
	}
	catch (std::exception const& error) {
		std::cerr << "FAIL " << error.what() << '\n';
		result = 1;
	}
	wxTheApp->OnExit();
	wxEntryCleanup();
	return result;
}
