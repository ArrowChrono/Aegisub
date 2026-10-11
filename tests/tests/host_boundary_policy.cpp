#include <main.h>

#include <libaegisub/fs.h>

#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// NOTE: This file is a SECOND SOURCE OF TRUTH for the core/GUI boundary. It
// hardcodes expected host filenames, CMake source-set names, and the wx-surface
// inventory, and asserts them against the actual CMakeLists.txt source sets and
// src/ contents. Renaming a host file, splitting a CMake set, or moving a wx
// surface REQUIRES updating the matching expectations here in lockstep. This
// coupling is intentional: the brittleness is the enforcement. When you change
// boundary structure, grep this file for the affected name and update the
// expectation alongside CMakeLists.txt.

namespace {

bool StartsWith(std::string const& value, std::string_view prefix) {
	return value.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string const& value, std::string_view suffix) {
	return value.size() >= suffix.size()
		&& value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::filesystem::path ProjectRoot() {
	return std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR).lexically_normal();
}

bool IsBoundaryCandidate(std::string const& relative_path) {
	auto filename = agi::fs::PathToString(agi::fs::PathFromString(relative_path).filename());
	if (filename == "app_runtime.cpp" || filename == "app_runtime.h")
		return true;
	if (StartsWith(filename, "headless_") && (EndsWith(filename, ".cpp") || EndsWith(filename, ".h")))
		return true;
	if (EndsWith(filename, "_service.cpp") || EndsWith(filename, "_service.h"))
		return true;
	if (EndsWith(filename, "_ops.cpp") || EndsWith(filename, "_ops.h"))
		return true;
	if (EndsWith(filename, "_host.cpp") || EndsWith(filename, "_host.h"))
		return true;
	return false;
}

bool IsPresentationContractSource(std::filesystem::path const& root, std::filesystem::path const& path) {
	auto relative = agi::fs::PathToGenericString(std::filesystem::relative(path, root));
	return StartsWith(relative, "src/presentation/")
		&& (EndsWith(relative, ".cpp") || EndsWith(relative, ".h"));
}

bool IsGridCoreSource(std::filesystem::path const& root, std::filesystem::path const& path) {
	auto relative = agi::fs::PathToGenericString(std::filesystem::relative(path, root));
	return StartsWith(relative, "src/grid_core/")
		&& (EndsWith(relative, ".cpp") || EndsWith(relative, ".h"));
}

std::vector<std::string> FindWxMarkers(std::filesystem::path const& path) {
	static const std::regex wx_token_pattern(R"(\bwx[A-Z][A-Za-z0-9_]*\b)");

	std::ifstream input(path);
	std::vector<std::string> hits;
	std::string line;
	int line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		auto comment = line.find("//");
		auto code = line.substr(0, comment);
		if (code.find("#include <wx/") != std::string::npos
			|| code.find("#include \"wx/") != std::string::npos
			|| std::regex_search(code, wx_token_pattern)) {
			std::ostringstream hit;
			hit << agi::fs::PathToGenericString(path) << ":" << line_number << ": " << line;
			hits.push_back(hit.str());
		}
	}
	return hits;
}

std::vector<std::string> FindUiFrameworkMarkers(std::filesystem::path const& path) {
	std::ifstream input(path);
	std::vector<std::string> hits;
	std::string line;
	int line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		auto comment = line.find("//");
		auto code = line.substr(0, comment);
		if (code.find("#include <wx/") != std::string::npos
			|| code.find("#include \"wx/") != std::string::npos
			|| code.find("#include <Q") != std::string::npos
			|| code.find("#include \"Q") != std::string::npos
			|| code.find("Avalonia") != std::string::npos
			|| code.find("wxString") != std::string::npos
			|| code.find("wxWindow") != std::string::npos
			|| code.find("wxBitmap") != std::string::npos
			|| code.find("wxDC") != std::string::npos
			|| code.find("QString") != std::string::npos
			|| code.find("QObject") != std::string::npos
			|| code.find("QWidget") != std::string::npos) {
			std::ostringstream hit;
			hit << agi::fs::PathToGenericString(path) << ":" << line_number << ": " << line;
			hits.push_back(hit.str());
		}
	}
	return hits;
}

std::vector<std::string> FindLiteralHits(std::filesystem::path const& path, std::string_view needle) {
	std::ifstream input(path);
	std::vector<std::string> hits;
	std::string line;
	int line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		if (line.find(needle) == std::string::npos)
			continue;

		std::ostringstream hit;
		hit << agi::fs::PathToGenericString(path) << ":" << line_number << ": " << line;
		hits.push_back(hit.str());
	}
	return hits;
}

std::vector<std::string> ReadCoreCApiExportedFunctionNames(std::filesystem::path const& header) {
	std::ifstream input(header);
	std::vector<std::string> names;
	std::string line;
	static const std::regex export_pattern(R"(AEGISUB_CORE_API\s+.*\bAEGISUB_CORE_CALL\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");

	while (std::getline(input, line)) {
		std::smatch match;
		if (std::regex_search(line, match, export_pattern))
			names.push_back(match[1].str());
	}
	return names;
}

std::set<std::string> FindFilesContainingLiteralInTree(
	std::filesystem::path const& project_root,
	std::filesystem::path const& search_root,
	std::string_view needle) {
	std::set<std::string> files;
	for (auto const& entry : std::filesystem::directory_iterator(search_root)) {
		if (!entry.is_regular_file())
			continue;

		auto const extension = agi::fs::PathToString(entry.path().extension());
		if (extension != ".cpp" && extension != ".h")
			continue;

		if (!FindLiteralHits(entry.path(), needle).empty())
			files.insert(agi::fs::PathToGenericString(std::filesystem::relative(entry.path(), project_root)));
	}
	return files;
}

std::vector<std::string> FindLiteralHitsRecursive(
	std::filesystem::path const& root,
	std::string_view needle) {
	std::vector<std::string> hits;
	for (auto const& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file())
			continue;

		auto const extension = agi::fs::PathToString(entry.path().extension());
		if (extension != ".cpp" && extension != ".h")
			continue;

		auto file_hits = FindLiteralHits(entry.path(), needle);
		hits.insert(hits.end(), file_hits.begin(), file_hits.end());
	}
	return hits;
}

std::string JoinLines(std::vector<std::string> const& lines) {
	std::ostringstream out;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i != 0)
			out << "\n";
		out << lines[i];
	}
	return out.str();
}

std::string TrimCopy(std::string value) {
	auto const not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

std::set<std::string> ReadNamedCMakeSetEntries(std::filesystem::path const& path, std::string const& variable_name) {
	std::ifstream input(path);
	std::set<std::string> entries;
	std::string line;
	bool in_block = false;
	auto const begin_marker = "set(" + variable_name;

	while (std::getline(input, line)) {
		auto trimmed = TrimCopy(line);
		if (!in_block) {
			if (trimmed == begin_marker)
				in_block = true;
			continue;
		}

		if (trimmed == ")")
			break;
		if (trimmed.empty() || StartsWith(trimmed, "#"))
			continue;
		entries.insert(trimmed);
	}

	return entries;
}

std::set<std::string> ReadCMakeTargetSourceEntries(std::filesystem::path const& path, std::string const& target_name) {
	std::ifstream input(path);
	std::set<std::string> entries;
	std::string line;
	bool in_block = false;
	auto const begin_marker = "target_sources(" + target_name;

	auto add_tokens = [&](std::string value) {
		for (auto& ch : value) {
			if (ch == '(' || ch == ')')
				ch = ' ';
		}

		std::istringstream tokens(value);
		std::string token;
		while (tokens >> token) {
			if (token == "target_sources" || token == target_name || token == "PRIVATE" || token == "PUBLIC" || token == "INTERFACE")
				continue;
			if (StartsWith(token, "src/") || StartsWith(token, "${"))
				entries.insert(token);
		}
	};

	while (std::getline(input, line)) {
		auto trimmed = TrimCopy(line);
		if (!in_block) {
			if (!StartsWith(trimmed, begin_marker))
				continue;

			in_block = true;
			add_tokens(trimmed);
			if (trimmed.find(')') != std::string::npos)
				in_block = false;
			continue;
		}

		add_tokens(trimmed);
		if (trimmed == ")" || trimmed.find(')') != std::string::npos)
			in_block = false;
	}

	return entries;
}

std::vector<std::string> ReadLocalIncludes(std::filesystem::path const& path) {
	static const std::regex include_pattern("^\\s*#\\s*include\\s+\"([^\"]+)\"");

	std::ifstream input(path);
	std::vector<std::string> includes;
	std::string line;
	while (std::getline(input, line)) {
		std::smatch match;
		if (std::regex_search(line, match, include_pattern))
			includes.push_back(match[1].str());
	}
	return includes;
}

std::filesystem::path ResolveSrcLocalInclude(
	std::filesystem::path const& root,
	std::filesystem::path const& including_file,
	std::string const& include) {
	std::vector<std::filesystem::path> const candidates = {
		including_file.parent_path() / include,
		root / "src" / include,
		root / "src" / "include" / include,
	};

	for (auto const& candidate : candidates) {
		auto normalized = candidate.lexically_normal();
		if (std::filesystem::exists(normalized)
			&& StartsWith(agi::fs::PathToGenericString(std::filesystem::relative(normalized, root)), "src/"))
			return normalized;
	}

	return {};
}

std::set<std::filesystem::path> BuildSrcLocalIncludeClosure(
	std::filesystem::path const& root,
	std::set<std::string> const& seed_sources) {
	std::set<std::filesystem::path> visited;
	std::deque<std::filesystem::path> pending;

	for (auto const& source : seed_sources) {
		if (!StartsWith(source, "src/"))
			continue;

		auto path = (root / std::filesystem::path(source)).lexically_normal();
		if (std::filesystem::exists(path))
			pending.push_back(path);
	}

	while (!pending.empty()) {
		auto path = pending.front();
		pending.pop_front();
		if (!visited.insert(path).second)
			continue;

		for (auto const& include : ReadLocalIncludes(path)) {
			auto resolved = ResolveSrcLocalInclude(root, path, include);
			if (!resolved.empty() && !visited.count(resolved))
				pending.push_back(resolved);
		}
	}

	return visited;
}

}

TEST(host_boundary_policy, service_like_sources_keep_wx_at_host_edges) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	std::set<std::string> const allowed_wx_candidates = {
		"src/gui_wx_locale_host.cpp",
		"src/gui_wx_runtime_entry_host.cpp",
		"src/gui_wx_runtime_host.cpp",
		"src/wx_audio_controller_power_host.cpp",
		"src/wx_frame_main_dialog_ui_host.h",
		"src/wx_frame_main_request_host.h",
		"src/wx_frame_main_runtime_host.h",
		"src/wx_preferences_ui_host.h",
		"src/wx_style_editor_ui_host.h",
		"src/wx_translation_service.h",
	};

	std::set<std::string> actual_wx_candidates;
	std::vector<std::string> unexpected_hits;
	for (auto const& entry : std::filesystem::directory_iterator(src_root)) {
		if (!entry.is_regular_file())
			continue;

		auto relative = agi::fs::PathToGenericString(std::filesystem::relative(entry.path(), root));
		if (!IsBoundaryCandidate(relative))
			continue;

		auto hits = FindWxMarkers(entry.path());
		if (hits.empty())
			continue;

		actual_wx_candidates.insert(relative);
		if (!allowed_wx_candidates.contains(relative))
			unexpected_hits.insert(unexpected_hits.end(), hits.begin(), hits.end());
	}

	EXPECT_TRUE(unexpected_hits.empty()) << JoinLines(unexpected_hits);
	EXPECT_EQ(allowed_wx_candidates, actual_wx_candidates);
}

TEST(host_boundary_policy, ui_service_contract_stays_split_from_wx_adapter) {
	auto const root = ProjectRoot();
	auto const ui_services_h = root / "src" / "ui_services.h";
	auto const ui_services_cpp = root / "src" / "ui_services.cpp";
	auto const wx_message_box_ui_services_h = root / "src" / "wx_message_box_ui_services.h";
	auto const wx_single_choice_dialog_h = root / "src" / "wx_single_choice_dialog.h";
	auto const wx_file_dialog_services_h = root / "src" / "wx_file_dialog_services.h";
	auto const legacy_wx_ui_services_h = root / "src" / "wx_ui_services.h";

	auto ui_header_hits = FindWxMarkers(ui_services_h);
	auto ui_cpp_hits = FindWxMarkers(ui_services_cpp);
	auto wx_message_box_hits = FindWxMarkers(wx_message_box_ui_services_h);
	auto wx_single_choice_hits = FindWxMarkers(wx_single_choice_dialog_h);
	auto wx_file_dialog_hits = FindWxMarkers(wx_file_dialog_services_h);

	EXPECT_TRUE(ui_header_hits.empty()) << JoinLines(ui_header_hits);
	EXPECT_TRUE(ui_cpp_hits.empty()) << JoinLines(ui_cpp_hits);
	EXPECT_FALSE(wx_message_box_hits.empty());
	EXPECT_FALSE(wx_single_choice_hits.empty());
	EXPECT_FALSE(wx_file_dialog_hits.empty());
	EXPECT_FALSE(std::filesystem::exists(legacy_wx_ui_services_h));
}

TEST(host_boundary_policy, presentation_contract_sources_stay_gui_framework_neutral) {
	auto const root = ProjectRoot();
	auto const presentation_root = root / "src" / "presentation";
	ASSERT_TRUE(std::filesystem::exists(presentation_root));

	std::vector<std::string> framework_hits;
	for (auto const& entry : std::filesystem::recursive_directory_iterator(presentation_root)) {
		if (!entry.is_regular_file() || !IsPresentationContractSource(root, entry.path()))
			continue;

		auto hits = FindUiFrameworkMarkers(entry.path());
		framework_hits.insert(framework_hits.end(), hits.begin(), hits.end());
	}

	EXPECT_TRUE(framework_hits.empty()) << JoinLines(framework_hits);
}

TEST(host_boundary_policy, grid_core_sources_stay_gui_framework_neutral) {
	auto const root = ProjectRoot();
	auto const grid_core_root = root / "src" / "grid_core";
	ASSERT_TRUE(std::filesystem::exists(grid_core_root));

	std::vector<std::string> framework_hits;
	for (auto const& entry : std::filesystem::recursive_directory_iterator(grid_core_root)) {
		if (!entry.is_regular_file() || !IsGridCoreSource(root, entry.path()))
			continue;

		auto hits = FindUiFrameworkMarkers(entry.path());
		framework_hits.insert(framework_hits.end(), hits.begin(), hits.end());
	}

	EXPECT_TRUE(framework_hits.empty()) << JoinLines(framework_hits);
}

TEST(host_boundary_policy, live_base_grid_consumes_presentation_projection_window) {
	auto const root = ProjectRoot();
	auto const base_grid_cpp = root / "src" / "base_grid.cpp";
	auto const base_grid_h = root / "src" / "base_grid.h";
	auto const grid_column_cpp = root / "src" / "grid_column.cpp";

	ASSERT_TRUE(std::filesystem::exists(base_grid_cpp));
	ASSERT_TRUE(std::filesystem::exists(base_grid_h));
	ASSERT_TRUE(std::filesystem::exists(grid_column_cpp));

	auto projection_include_hits = FindLiteralHits(base_grid_cpp, "presentation/subtitle_grid_projection.h");
	auto diff_include_hits = FindLiteralHits(base_grid_cpp, "presentation/subtitle_grid_diff.h");
	auto window_type_hits = FindLiteralHits(base_grid_h, "SubtitleGridWindow");
	auto row_state_hits = FindLiteralHits(base_grid_h, "SubtitleGridRowState");
	auto build_window_hits = FindLiteralHits(base_grid_cpp, "BuildSubtitleGridWindow(");
	auto build_diff_hits = FindLiteralHits(base_grid_cpp, "BuildSubtitleGridDiffFromCommit(");
	auto query_adapter_hits = FindLiteralHits(base_grid_cpp, "BaseGrid::QueryGridWindow(");
	auto width_column_ids_hits = FindLiteralHits(base_grid_cpp, "BaseGrid::ProjectionColumnIdsForWidths(");
	auto width_window_hits = FindLiteralHits(base_grid_cpp, "column->UpdateWidth(context, *width_helper, width_window)");
	auto timing_local_queue_hits = FindLiteralHits(base_grid_cpp, "QueueSubtitleGridRowRefresh(single_line->Row)");
	auto timing_visible_diff_hits = FindLiteralHits(base_grid_cpp, "QueueChangedVisibleRowsRefresh(visible_rows, new_visible_rows)");
	auto timing_active_window_hits = FindLiteralHits(base_grid_cpp, "QueueVisibleWindowRefresh()");
	auto collision_state_hits = FindLiteralHits(base_grid_cpp, "row_state.collides_with_active");
	auto projected_row_map_hits = FindLiteralHits(base_grid_cpp, "projected_rows_by_screen_row");
	auto projected_row_paint_hits = FindLiteralHits(base_grid_cpp, "columns[j]->Paint(*painter, x, y, *projected_row, context)");
	auto string_width_cache_hits = FindLiteralHits(grid_column_cpp, "return (*this)(boost::flyweight<std::string>(str));");
	auto grid_column_formatter_hits = FindLiteralHits(grid_column_cpp, "FormatSubtitleGridCell(row, ProjectionColumnId()");
	auto grid_column_ass_scan_hits = FindLiteralHits(grid_column_cpp, "core.ass->Events");
	auto grid_column_ass_paint_hits = FindLiteralHits(grid_column_cpp, "const AssDialogue *");
	auto grid_column_ass_value_hits = FindLiteralHits(grid_column_cpp, "Value(const AssDialogue");

	EXPECT_FALSE(projection_include_hits.empty());
	EXPECT_FALSE(diff_include_hits.empty());
	EXPECT_FALSE(window_type_hits.empty());
	EXPECT_FALSE(row_state_hits.empty());
	EXPECT_FALSE(build_window_hits.empty());
	EXPECT_FALSE(build_diff_hits.empty());
	EXPECT_FALSE(query_adapter_hits.empty());
	EXPECT_FALSE(width_column_ids_hits.empty());
	EXPECT_FALSE(width_window_hits.empty());
	EXPECT_FALSE(timing_local_queue_hits.empty());
	EXPECT_FALSE(timing_visible_diff_hits.empty());
	EXPECT_FALSE(timing_active_window_hits.empty());
	EXPECT_FALSE(collision_state_hits.empty());
	EXPECT_FALSE(projected_row_map_hits.empty());
	EXPECT_FALSE(projected_row_paint_hits.empty());
	EXPECT_FALSE(string_width_cache_hits.empty());
	EXPECT_FALSE(grid_column_formatter_hits.empty());
	EXPECT_TRUE(grid_column_ass_scan_hits.empty()) << JoinLines(grid_column_ass_scan_hits);
	EXPECT_TRUE(grid_column_ass_paint_hits.empty()) << JoinLines(grid_column_ass_paint_hits);
	EXPECT_TRUE(grid_column_ass_value_hits.empty()) << JoinLines(grid_column_ass_value_hits);
}

TEST(host_boundary_policy, context_backed_file_dialog_callers_prefer_context_service) {
	auto const root = ProjectRoot();

	struct FileDialogCallerExpectation {
		std::string path;
		std::vector<std::string> required_context_calls;
	};

	std::vector<FileDialogCallerExpectation> const expectations = {
		{
			"src/dialog_attachments.cpp",
			{"RequestOpenFiles(", "RequestSaveFile(", "RequestSelectDirectory("}
		},
		{
			"src/dialog_automation.cpp",
			{"RequestOpenFiles("}
		},
		{
			"src/dialog_fonts_collector.cpp",
			{"RequestSaveFile(", "RequestSelectDirectory("}
		},
	};

	for (auto const& expectation : expectations) {
		auto const path = root / expectation.path;
		ASSERT_TRUE(std::filesystem::exists(path)) << expectation.path;

		auto include_hits = FindLiteralHits(path, "wx_file_dialog_services.h");
		auto make_window_hits = FindLiteralHits(path, "MakeWindowFileDialogService(");
		EXPECT_TRUE(include_hits.empty()) << JoinLines(include_hits);
		EXPECT_TRUE(make_window_hits.empty()) << JoinLines(make_window_hits);

		for (auto const& call : expectation.required_context_calls) {
			auto call_hits = FindLiteralHits(path, call);
			EXPECT_FALSE(call_hits.empty()) << expectation.path << " missing " << call;
		}
	}
}

TEST(host_boundary_policy, preferences_file_dialog_usage_stays_centralized) {
	auto const root = ProjectRoot();
	auto const preferences_cpp = root / "src" / "preferences.cpp";
	auto const preferences_base_cpp = root / "src" / "preferences_base.cpp";
	auto const wx_preferences_ui_host_h = root / "src" / "wx_preferences_ui_host.h";

	auto preferences_include_hits = FindLiteralHits(preferences_cpp, "wx_file_dialog_services.h");
	auto preferences_make_hits = FindLiteralHits(preferences_cpp, "MakeWindowFileDialogService(");
	auto preferences_seam_hits = FindLiteralHits(preferences_cpp, "MakePreferencesFileDialogService(");
	auto preferences_request_hits = FindLiteralHits(preferences_cpp, "RequestSelectDirectory(");
	auto preferences_open_hits = FindLiteralHits(preferences_cpp, "RequestOpenFile(");

	auto preferences_base_include_hits = FindLiteralHits(preferences_base_cpp, "wx_file_dialog_services.h");
	auto preferences_base_make_hits = FindLiteralHits(preferences_base_cpp, "MakeWindowFileDialogService(");
	auto preferences_base_request_hits = FindLiteralHits(preferences_base_cpp, "RequestSelectDirectory(");
	auto preferences_base_open_hits = FindLiteralHits(preferences_base_cpp, "RequestOpenFile(");

	auto seam_include_hits = FindLiteralHits(wx_preferences_ui_host_h, "wx_file_dialog_services.h");
	auto seam_make_hits = FindLiteralHits(wx_preferences_ui_host_h, "MakeWindowFileDialogService(");

	EXPECT_TRUE(preferences_include_hits.empty()) << JoinLines(preferences_include_hits);
	EXPECT_TRUE(preferences_make_hits.empty()) << JoinLines(preferences_make_hits);
	EXPECT_FALSE(preferences_seam_hits.empty());
	EXPECT_FALSE(preferences_request_hits.empty());
	EXPECT_FALSE(preferences_open_hits.empty());

	EXPECT_TRUE(preferences_base_include_hits.empty()) << JoinLines(preferences_base_include_hits);
	EXPECT_TRUE(preferences_base_make_hits.empty()) << JoinLines(preferences_base_make_hits);
	EXPECT_FALSE(preferences_base_request_hits.empty());
	EXPECT_FALSE(preferences_base_open_hits.empty());

	EXPECT_FALSE(seam_include_hits.empty());
	EXPECT_FALSE(seam_make_hits.empty());
}

TEST(host_boundary_policy, automation_file_dialog_requests_use_neutral_host_service) {
	auto const root = ProjectRoot();
	auto const auto4_lua_cpp = root / "src" / "auto4_lua.cpp";
	auto const auto4_progress_cpp = root / "src" / "auto4_lua_progresssink.cpp";

	auto auto4_include_hits = FindLiteralHits(auto4_lua_cpp, "wx_file_dialog_services.h");
	auto auto4_make_hits = FindLiteralHits(auto4_lua_cpp, "MakeWindowFileDialogService(");
	auto auto4_seam_hits = FindLiteralHits(auto4_lua_cpp, "ResolveAutomationFileDialogService(");

	auto host_open_hits = FindLiteralHits(auto4_progress_cpp, "host->Ui().RequestOpenFiles(");
	auto host_save_hits = FindLiteralHits(auto4_progress_cpp, "host->Ui().RequestSaveFile(");

	EXPECT_TRUE(auto4_include_hits.empty()) << JoinLines(auto4_include_hits);
	EXPECT_TRUE(auto4_make_hits.empty()) << JoinLines(auto4_make_hits);
	EXPECT_TRUE(auto4_seam_hits.empty()) << JoinLines(auto4_seam_hits);

	EXPECT_FALSE(host_open_hits.empty());
	EXPECT_FALSE(host_save_hits.empty());
}

TEST(host_boundary_policy, app_bootstrap_ui_fallback_lives_in_explicit_wx_service_seam) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const gui_wx_bootstrap_ui_host_h = root / "src" / "gui_wx_bootstrap_ui_host.h";
	auto const gui_wx_bootstrap_ui_host_cpp = root / "src" / "gui_wx_bootstrap_ui_host.cpp";
	auto const wx_app_bootstrap_ui_services_h = root / "src" / "wx_app_bootstrap_ui_services.h";

	auto main_message_box_include_hits = FindLiteralHits(main_cpp, "wx_message_box_ui_services.h");
	auto main_single_choice_include_hits = FindLiteralHits(main_cpp, "wx_single_choice_dialog.h");
	auto main_notification_hits = FindLiteralHits(main_cpp, "WxMessageBoxNotificationSink");
	auto main_interaction_hits = FindLiteralHits(main_cpp, "WxMessageBoxInteractionSink");
	auto main_make_single_choice_hits = FindLiteralHits(main_cpp, "MakeWindowSingleChoiceInteractionSink(");
	auto main_bootstrap_ui_include_hits = FindLiteralHits(main_cpp, "wx_app_bootstrap_ui_services.h");
	auto main_runtime_entry_host_include_hits = FindLiteralHits(main_cpp, "gui_wx_runtime_entry_host.h");
	auto main_gui_bootstrap_ui_host_include_hits = FindLiteralHits(main_cpp, "gui_wx_bootstrap_ui_host.h");
	auto main_build_runtime_entry_host_pack_hits = FindLiteralHits(main_cpp, "BuildGuiWxRuntimeEntryHostPack()");
	auto main_build_runtime_init_options_hits = FindLiteralHits(main_cpp, "BuildGuiWxAppRuntimeInitOptions()");
	auto main_build_bootstrap_ui_host_hits = FindLiteralHits(main_cpp, "BuildGuiWxRuntimeBootstrapUiHost()");
	auto main_direct_notification_field_hits = FindLiteralHits(main_cpp, ".notification_sink");
	auto main_direct_interaction_field_hits = FindLiteralHits(main_cpp, ".interaction_sink");
	auto main_bootstrap_error_helper_hits = FindLiteralHits(main_cpp, "ShowBootstrapUiError(");
	auto main_bootstrap_interaction_helper_hits = FindLiteralHits(main_cpp, "RequestBootstrapUiInteraction(");
	auto main_gui_bootstrap_error_helper_hits = FindLiteralHits(main_cpp, "ShowGuiWxBootstrapUiError(");
	auto main_gui_bootstrap_interaction_helper_hits = FindLiteralHits(main_cpp, "RequestGuiWxBootstrapUiInteraction(");
	auto host_runtime_bootstrap_host_include_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_h, "runtime_bootstrap_ui_host.h");
	auto host_bootstrap_ui_include_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "wx_app_bootstrap_ui_services.h");
	auto host_bootstrap_notification_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "AppBootstrapNotificationSink()");
	auto host_bootstrap_interaction_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "AppBootstrapInteractionSink()");
	auto host_bootstrap_single_choice_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "MakeAppBootstrapSingleChoiceInteractionSink()");

	auto seam_notification_hits = FindLiteralHits(wx_app_bootstrap_ui_services_h, "WxMessageBoxNotificationSink");
	auto seam_interaction_hits = FindLiteralHits(wx_app_bootstrap_ui_services_h, "WxMessageBoxInteractionSink");
	auto seam_single_choice_hits = FindLiteralHits(wx_app_bootstrap_ui_services_h, "MakeWindowSingleChoiceInteractionSink(");

	EXPECT_TRUE(main_message_box_include_hits.empty()) << JoinLines(main_message_box_include_hits);
	EXPECT_TRUE(main_single_choice_include_hits.empty()) << JoinLines(main_single_choice_include_hits);
	EXPECT_TRUE(main_notification_hits.empty()) << JoinLines(main_notification_hits);
	EXPECT_TRUE(main_interaction_hits.empty()) << JoinLines(main_interaction_hits);
	EXPECT_TRUE(main_make_single_choice_hits.empty()) << JoinLines(main_make_single_choice_hits);
	EXPECT_TRUE(main_bootstrap_ui_include_hits.empty()) << JoinLines(main_bootstrap_ui_include_hits);
	EXPECT_FALSE(main_runtime_entry_host_include_hits.empty());
	EXPECT_TRUE(main_gui_bootstrap_ui_host_include_hits.empty()) << JoinLines(main_gui_bootstrap_ui_host_include_hits);
	EXPECT_TRUE(main_build_runtime_entry_host_pack_hits.empty()) << JoinLines(main_build_runtime_entry_host_pack_hits);
	EXPECT_FALSE(main_build_runtime_init_options_hits.empty());
	EXPECT_TRUE(main_build_bootstrap_ui_host_hits.empty()) << JoinLines(main_build_bootstrap_ui_host_hits);
	EXPECT_TRUE(main_direct_notification_field_hits.empty()) << JoinLines(main_direct_notification_field_hits);
	EXPECT_TRUE(main_direct_interaction_field_hits.empty()) << JoinLines(main_direct_interaction_field_hits);
	EXPECT_TRUE(main_bootstrap_error_helper_hits.empty()) << JoinLines(main_bootstrap_error_helper_hits);
	EXPECT_TRUE(main_bootstrap_interaction_helper_hits.empty()) << JoinLines(main_bootstrap_interaction_helper_hits);
	EXPECT_FALSE(main_gui_bootstrap_error_helper_hits.empty());
	EXPECT_TRUE(main_gui_bootstrap_interaction_helper_hits.empty()) << JoinLines(main_gui_bootstrap_interaction_helper_hits);

	EXPECT_FALSE(host_runtime_bootstrap_host_include_hits.empty());
	EXPECT_FALSE(host_bootstrap_ui_include_hits.empty());
	EXPECT_FALSE(host_bootstrap_notification_hits.empty());
	EXPECT_FALSE(host_bootstrap_interaction_hits.empty());
	EXPECT_FALSE(host_bootstrap_single_choice_hits.empty());

	EXPECT_FALSE(seam_notification_hits.empty());
	EXPECT_FALSE(seam_interaction_hits.empty());
	EXPECT_FALSE(seam_single_choice_hits.empty());
}

TEST(host_boundary_policy, gui_startup_orchestration_stays_localized_to_runtime_entry_host) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const gui_runtime_entry_host_cpp = root / "src" / "gui_wx_runtime_entry_host.cpp";

	auto main_startup_helper_hits = FindLiteralHits(main_cpp, "RunGuiWxAppStartupSequence(");
	auto main_first_start_hits = FindLiteralHits(main_cpp, "OPT_GET(\"App/First Start\")");
	auto main_update_check_hits = FindLiteralHits(main_cpp, "PerformVersionCheck(false)");
	auto main_bootstrap_interaction_hits = FindLiteralHits(main_cpp, "RequestGuiWxBootstrapUiInteraction(");
	auto main_config_flush_hits = FindLiteralHits(main_cpp, "config::opt->Flush()");

	auto entry_host_startup_helper_hits = FindLiteralHits(gui_runtime_entry_host_cpp, "RunGuiWxAppStartupSequence(");
	auto entry_host_first_start_hits = FindLiteralHits(gui_runtime_entry_host_cpp, "OPT_GET(\"App/First Start\")");
	auto entry_host_update_check_hits = FindLiteralHits(gui_runtime_entry_host_cpp, "PerformVersionCheck(false)");
	auto entry_host_bootstrap_interaction_hits = FindLiteralHits(gui_runtime_entry_host_cpp, "RequestGuiWxBootstrapUiInteraction(");
	auto entry_host_config_flush_hits = FindLiteralHits(gui_runtime_entry_host_cpp, "config::opt->Flush()");

	EXPECT_FALSE(main_startup_helper_hits.empty());
	EXPECT_TRUE(main_first_start_hits.empty()) << JoinLines(main_first_start_hits);
	EXPECT_TRUE(main_update_check_hits.empty()) << JoinLines(main_update_check_hits);
	EXPECT_TRUE(main_bootstrap_interaction_hits.empty()) << JoinLines(main_bootstrap_interaction_hits);
	EXPECT_TRUE(main_config_flush_hits.empty()) << JoinLines(main_config_flush_hits);

	EXPECT_FALSE(entry_host_startup_helper_hits.empty());
	EXPECT_FALSE(entry_host_first_start_hits.empty());
	EXPECT_FALSE(entry_host_update_check_hits.empty());
	EXPECT_FALSE(entry_host_bootstrap_interaction_hits.empty());
	EXPECT_FALSE(entry_host_config_flush_hits.empty());
}

TEST(host_boundary_policy, preferences_interaction_usage_stays_centralized) {
	auto const root = ProjectRoot();
	auto const preferences_cpp = root / "src" / "preferences.cpp";
	auto const wx_preferences_ui_host_h = root / "src" / "wx_preferences_ui_host.h";

	auto include_hits = FindLiteralHits(preferences_cpp, "wx_message_box_ui_services.h");
	auto make_hits = FindLiteralHits(preferences_cpp, "MakeWindowInteractionSink(");
	auto seam_hits = FindLiteralHits(preferences_cpp, "MakePreferencesInteractionSink(");
	auto request_hits = FindLiteralHits(preferences_cpp, "RequestInteraction(");
	auto seam_include_hits = FindLiteralHits(wx_preferences_ui_host_h, "wx_message_box_ui_services.h");
	auto seam_make_hits = FindLiteralHits(wx_preferences_ui_host_h, "MakeWindowInteractionSink(");

	EXPECT_TRUE(include_hits.empty()) << JoinLines(include_hits);
	EXPECT_TRUE(make_hits.empty()) << JoinLines(make_hits);
	EXPECT_FALSE(seam_hits.empty());
	EXPECT_FALSE(request_hits.empty());
	EXPECT_FALSE(seam_include_hits.empty());
	EXPECT_FALSE(seam_make_hits.empty());
}

TEST(host_boundary_policy, style_editor_ui_fallback_lives_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const dialog_style_editor_cpp = root / "src" / "dialog_style_editor.cpp";
	auto const wx_style_editor_ui_host_h = root / "src" / "wx_style_editor_ui_host.h";

	auto include_hits = FindLiteralHits(dialog_style_editor_cpp, "wx_message_box_ui_services.h");
	auto make_notification_hits = FindLiteralHits(dialog_style_editor_cpp, "MakeWindowNotificationSink(");
	auto make_interaction_hits = FindLiteralHits(dialog_style_editor_cpp, "MakeWindowInteractionSink(");
	auto seam_notification_hits = FindLiteralHits(dialog_style_editor_cpp, "ResolveStyleEditorNotificationSink(");
	auto seam_interaction_hits = FindLiteralHits(dialog_style_editor_cpp, "ResolveStyleEditorInteractionSink(");

	auto seam_make_notification_hits = FindLiteralHits(wx_style_editor_ui_host_h, "MakeWindowNotificationSink(");
	auto seam_make_interaction_hits = FindLiteralHits(wx_style_editor_ui_host_h, "MakeWindowInteractionSink(");
	auto seam_include_hits = FindLiteralHits(wx_style_editor_ui_host_h, "wx_message_box_ui_services.h");

	EXPECT_TRUE(include_hits.empty()) << JoinLines(include_hits);
	EXPECT_TRUE(make_notification_hits.empty()) << JoinLines(make_notification_hits);
	EXPECT_TRUE(make_interaction_hits.empty()) << JoinLines(make_interaction_hits);
	EXPECT_FALSE(seam_notification_hits.empty());
	EXPECT_FALSE(seam_interaction_hits.empty());

	EXPECT_FALSE(seam_make_notification_hits.empty());
	EXPECT_FALSE(seam_make_interaction_hits.empty());
	EXPECT_FALSE(seam_include_hits.empty());
}

TEST(host_boundary_policy, frame_main_dialog_ui_fallback_lives_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const frame_main_cpp = root / "src" / "frame_main.cpp";
	auto const wx_frame_main_dialog_ui_host_h = root / "src" / "wx_frame_main_dialog_ui_host.h";

	auto message_box_include_hits = FindLiteralHits(frame_main_cpp, "wx_message_box_ui_services.h");
	auto single_choice_include_hits = FindLiteralHits(frame_main_cpp, "wx_single_choice_dialog.h");
	auto notification_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainNotificationSink");
	auto interaction_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainInteractionSink");
	auto single_choice_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainSingleChoiceInteractionSink");
	auto startup_log_hits = FindLiteralHits(frame_main_cpp, "ShowFrameMainStartupLogDialog(");
	auto seam_notification_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainNotificationSink(");
	auto seam_interaction_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainInteractionSink(");
	auto seam_single_choice_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainSingleChoiceInteractionSink(");

	auto seam_message_box_include_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "wx_message_box_ui_services.h");
	auto seam_single_choice_include_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "wx_single_choice_dialog.h");
	auto seam_message_box_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "wxMessageBox(");
	auto seam_single_choice_dialog_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "ShowSingleChoiceDialog(");

	EXPECT_TRUE(message_box_include_hits.empty()) << JoinLines(message_box_include_hits);
	EXPECT_TRUE(single_choice_include_hits.empty()) << JoinLines(single_choice_include_hits);
	EXPECT_TRUE(notification_class_hits.empty()) << JoinLines(notification_class_hits);
	EXPECT_TRUE(interaction_class_hits.empty()) << JoinLines(interaction_class_hits);
	EXPECT_TRUE(single_choice_class_hits.empty()) << JoinLines(single_choice_class_hits);
	EXPECT_FALSE(startup_log_hits.empty());
	EXPECT_FALSE(seam_notification_hits.empty());
	EXPECT_FALSE(seam_interaction_hits.empty());
	EXPECT_FALSE(seam_single_choice_hits.empty());

	EXPECT_FALSE(seam_message_box_include_hits.empty());
	EXPECT_FALSE(seam_single_choice_include_hits.empty());
	EXPECT_FALSE(seam_message_box_hits.empty());
	EXPECT_FALSE(seam_single_choice_dialog_hits.empty());
}

TEST(host_boundary_policy, frame_main_request_selection_fallback_lives_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const frame_main_cpp = root / "src" / "frame_main.cpp";
	auto const wx_frame_main_request_host_h = root / "src" / "wx_frame_main_request_host.h";

	auto file_dialog_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainFileDialogService");
	auto video_source_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainVideoSourceRequestService");
	auto file_drop_target_class_hits = FindLiteralHits(frame_main_cpp, "class AegisubFileDropTarget");
	auto open_file_hits = FindLiteralHits(frame_main_cpp, "OpenFileSelector(");
	auto open_files_hits = FindLiteralHits(frame_main_cpp, "OpenFilesSelector(");
	auto save_file_hits = FindLiteralHits(frame_main_cpp, "SaveFileSelector(");
	auto select_directory_hits = FindLiteralHits(frame_main_cpp, "SelectDirectorySelector(");
	auto dummy_video_hits = FindLiteralHits(frame_main_cpp, "CreateDummyVideo(");
	auto set_drop_target_inline_hits = FindLiteralHits(frame_main_cpp, "SetDropTarget(new AegisubFileDropTarget");
	auto seam_file_dialog_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainFileDialogService(");
	auto seam_video_source_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainVideoSourceRequestService(");
	auto seam_drop_target_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainFileDropTarget(");

	auto seam_wx_file_dialog_include_hits = FindLiteralHits(wx_frame_main_request_host_h, "wx_file_dialog_services.h");
	auto seam_make_window_file_dialog_hits = FindLiteralHits(wx_frame_main_request_host_h, "MakeWindowFileDialogService(");
	auto seam_dummy_video_hits = FindLiteralHits(wx_frame_main_request_host_h, "CreateDummyVideo(");
	auto seam_drop_target_class_hits = FindLiteralHits(wx_frame_main_request_host_h, "class WxFrameMainFileDropTarget");
	auto seam_drop_target_wx_hits = FindLiteralHits(wx_frame_main_request_host_h, "wxFileDropTarget");

	EXPECT_TRUE(file_dialog_class_hits.empty()) << JoinLines(file_dialog_class_hits);
	EXPECT_TRUE(video_source_class_hits.empty()) << JoinLines(video_source_class_hits);
	EXPECT_TRUE(file_drop_target_class_hits.empty()) << JoinLines(file_drop_target_class_hits);
	EXPECT_TRUE(open_file_hits.empty()) << JoinLines(open_file_hits);
	EXPECT_TRUE(open_files_hits.empty()) << JoinLines(open_files_hits);
	EXPECT_TRUE(save_file_hits.empty()) << JoinLines(save_file_hits);
	EXPECT_TRUE(select_directory_hits.empty()) << JoinLines(select_directory_hits);
	EXPECT_TRUE(dummy_video_hits.empty()) << JoinLines(dummy_video_hits);
	EXPECT_TRUE(set_drop_target_inline_hits.empty()) << JoinLines(set_drop_target_inline_hits);
	EXPECT_FALSE(seam_file_dialog_hits.empty());
	EXPECT_FALSE(seam_video_source_hits.empty());
	EXPECT_FALSE(seam_drop_target_hits.empty());

	EXPECT_FALSE(seam_wx_file_dialog_include_hits.empty());
	EXPECT_FALSE(seam_make_window_file_dialog_hits.empty());
	EXPECT_FALSE(seam_dummy_video_hits.empty());
	EXPECT_FALSE(seam_drop_target_class_hits.empty());
	EXPECT_FALSE(seam_drop_target_wx_hits.empty());
}

TEST(host_boundary_policy, frame_main_runtime_capabilities_live_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const frame_main_cpp = root / "src" / "frame_main.cpp";
	auto const wx_frame_main_runtime_host_h = root / "src" / "wx_frame_main_runtime_host.h";

	auto background_runner_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainBackgroundRunner");
	auto background_runner_factory_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainBackgroundRunnerFactory");
	auto project_ui_state_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainProjectUiStateSink");
	auto status_sink_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainStatusSink");
	auto audio_player_factory_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainAudioPlayerFactoryService");
	auto automation_runner_factory_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainAutomationBackgroundScriptRunnerFactory");
	auto dialog_progress_hits = FindLiteralHits(frame_main_cpp, "DialogProgress");
	auto audio_player_factory_hits = FindLiteralHits(frame_main_cpp, "AudioPlayerFactory::GetAudioPlayer(");
	auto automation_runner_hits = FindLiteralHits(frame_main_cpp, "Automation4::BackgroundScriptRunner");
	auto seam_background_runner_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainBackgroundRunnerFactory(");
	auto seam_status_sink_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainStatusSink(");
	auto seam_project_ui_state_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainProjectUiStateSink(");
	auto seam_audio_player_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainAudioPlayerFactoryService(");
	auto seam_automation_runner_factory_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainAutomationBackgroundScriptRunnerFactory(");

	auto seam_status_sink_class_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "class WxFrameMainStatusSink");
	auto seam_status_sink_header_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "status_sink.h");
	auto seam_dialog_progress_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "DialogProgress");
	auto seam_audio_player_factory_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "AudioPlayerFactory::GetAudioPlayer(");
	auto seam_automation_runner_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "Automation4::BackgroundScriptRunner");
	auto seam_file_dialog_factory_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "MakeFrameMainFileDialogService(");

	EXPECT_TRUE(background_runner_class_hits.empty()) << JoinLines(background_runner_class_hits);
	EXPECT_TRUE(background_runner_factory_class_hits.empty()) << JoinLines(background_runner_factory_class_hits);
	EXPECT_TRUE(project_ui_state_class_hits.empty()) << JoinLines(project_ui_state_class_hits);
	EXPECT_TRUE(status_sink_class_hits.empty()) << JoinLines(status_sink_class_hits);
	EXPECT_TRUE(audio_player_factory_class_hits.empty()) << JoinLines(audio_player_factory_class_hits);
	EXPECT_TRUE(automation_runner_factory_class_hits.empty()) << JoinLines(automation_runner_factory_class_hits);
	EXPECT_TRUE(dialog_progress_hits.empty()) << JoinLines(dialog_progress_hits);
	EXPECT_TRUE(audio_player_factory_hits.empty()) << JoinLines(audio_player_factory_hits);
	EXPECT_TRUE(automation_runner_hits.empty()) << JoinLines(automation_runner_hits);
	EXPECT_FALSE(seam_background_runner_hits.empty());
	EXPECT_FALSE(seam_status_sink_hits.empty());
	EXPECT_FALSE(seam_project_ui_state_hits.empty());
	EXPECT_FALSE(seam_audio_player_hits.empty());
	EXPECT_FALSE(seam_automation_runner_factory_hits.empty());

	EXPECT_FALSE(seam_status_sink_class_hits.empty());
	EXPECT_FALSE(seam_status_sink_header_hits.empty());
	EXPECT_FALSE(seam_dialog_progress_hits.empty());
	EXPECT_FALSE(seam_audio_player_factory_hits.empty());
	EXPECT_FALSE(seam_automation_runner_hits.empty());
	EXPECT_FALSE(seam_file_dialog_factory_hits.empty());
}

TEST(host_boundary_policy, audio_player_factory_contract_stays_wx_free) {
	auto const root = ProjectRoot();
	auto const audio_player_h = root / "src" / "include" / "aegisub" / "audio_player.h";
	auto const audio_player_cpp = root / "src" / "audio_player.cpp";
	auto const audio_player_alsa_cpp = root / "src" / "audio_player_alsa.cpp";
	auto const audio_player_dsound_cpp = root / "src" / "audio_player_dsound.cpp";
	auto const audio_player_dsound2_cpp = root / "src" / "audio_player_dsound2.cpp";
	auto const wx_frame_main_runtime_host_h = root / "src" / "wx_frame_main_runtime_host.h";

	auto audio_player_header_wx_hits = FindWxMarkers(audio_player_h);
	auto audio_player_cpp_wx_hits = FindWxMarkers(audio_player_cpp);
	auto audio_player_header_wx_window_hits = FindLiteralHits(audio_player_h, "wxWindow");
	auto audio_player_cpp_wx_window_hits = FindLiteralHits(audio_player_cpp, "wxWindow");
	auto audio_player_host_hits = FindLiteralHits(audio_player_h, "AudioPlayerHost");
	auto audio_player_factory_host_hits = FindLiteralHits(audio_player_cpp, "AudioPlayerHost");
	auto alsa_frame_main_include_hits = FindLiteralHits(audio_player_alsa_cpp, "frame_main.h");
	auto dsound_frame_main_include_hits = FindLiteralHits(audio_player_dsound_cpp, "frame_main.h");
	auto dsound2_frame_main_include_hits = FindLiteralHits(audio_player_dsound2_cpp, "frame_main.h");
	auto wx_runtime_host_native_parent_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "native_parent_handle");

	EXPECT_TRUE(audio_player_header_wx_hits.empty()) << JoinLines(audio_player_header_wx_hits);
	EXPECT_TRUE(audio_player_cpp_wx_hits.empty()) << JoinLines(audio_player_cpp_wx_hits);
	EXPECT_TRUE(audio_player_header_wx_window_hits.empty()) << JoinLines(audio_player_header_wx_window_hits);
	EXPECT_TRUE(audio_player_cpp_wx_window_hits.empty()) << JoinLines(audio_player_cpp_wx_window_hits);
	EXPECT_FALSE(audio_player_host_hits.empty());
	EXPECT_FALSE(audio_player_factory_host_hits.empty());
	EXPECT_TRUE(alsa_frame_main_include_hits.empty()) << JoinLines(alsa_frame_main_include_hits);
	EXPECT_TRUE(dsound_frame_main_include_hits.empty()) << JoinLines(dsound_frame_main_include_hits);
	EXPECT_TRUE(dsound2_frame_main_include_hits.empty()) << JoinLines(dsound2_frame_main_include_hits);
	EXPECT_FALSE(wx_runtime_host_native_parent_hits.empty());
}

TEST(host_boundary_policy, shared_dispatch_timers_stay_wx_free_and_no_longer_use_host_suffix) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";

	std::set<std::string> const expected_shared_timer_sources = {
		"src/audio_controller_timer.cpp",
		"src/audio_controller_timer.h",
		"src/main_thread_timer.h",
		"src/playback_probe_timer.cpp",
		"src/playback_probe_timer.h",
		"src/playback_session_timer.cpp",
		"src/playback_session_timer.h",
		"src/video_controller_timer.cpp",
		"src/video_controller_timer.h",
	};

	std::vector<std::string> unexpected_legacy_host_files;
	for (auto const& entry : std::filesystem::directory_iterator(src_root)) {
		if (!entry.is_regular_file())
			continue;
		auto const filename = agi::fs::PathToString(entry.path().filename());
		if (filename.find("_timer_host.") == std::string::npos)
			continue;

		std::ostringstream hit;
		hit << agi::fs::PathToGenericString(std::filesystem::relative(entry.path(), root));
		unexpected_legacy_host_files.push_back(hit.str());
	}

	std::vector<std::string> unexpected_wx_hits;
	for (auto const& relative : expected_shared_timer_sources) {
		auto const path = root / std::filesystem::path(relative);
		ASSERT_TRUE(std::filesystem::exists(path)) << relative;

		auto hits = FindWxMarkers(path);
		unexpected_wx_hits.insert(unexpected_wx_hits.end(), hits.begin(), hits.end());
	}

	EXPECT_TRUE(unexpected_legacy_host_files.empty()) << JoinLines(unexpected_legacy_host_files);
	EXPECT_TRUE(unexpected_wx_hits.empty()) << JoinLines(unexpected_wx_hits);
}

TEST(host_boundary_policy, runtime_wx_hooks_live_in_explicit_runtime_host_files) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const gui_locale_host_cpp = root / "src" / "gui_wx_locale_host.cpp";
	auto const gui_runtime_host_cpp = root / "src" / "gui_wx_runtime_host.cpp";
	auto const aegisublocale_cpp = root / "src" / "aegisublocale.cpp";
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";
	auto const legacy_headless_runtime_host_cpp = root / "src" / "headless_wx_runtime_host.cpp";

	auto main_log_hits = FindLiteralHits(main_cpp, "wxLog::GetActiveTarget");
	auto main_png_hits = FindLiteralHits(main_cpp, "wxPNGHandler");
	auto main_locale_host_hits = FindLiteralHits(main_cpp, "wxTranslations");
	auto gui_log_hits = FindLiteralHits(gui_runtime_host_cpp, "wxLog::GetActiveTarget");
	auto gui_png_hits = FindLiteralHits(gui_runtime_host_cpp, "wxPNGHandler");
	auto gui_locale_translation_hits = FindLiteralHits(gui_locale_host_cpp, "wxTranslations");
	auto gui_locale_catalog_hits = FindLiteralHits(gui_locale_host_cpp, "AddCatalogLookupPathPrefix");
	auto locale_cpp_wx_hits = FindWxMarkers(aegisublocale_cpp);
	auto headless_log_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxLog::GetActiveTarget");
	auto headless_png_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxPNGHandler");
	auto headless_locale_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxTranslations");
	auto headless_host_include_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_wx_runtime_host.h");

	EXPECT_TRUE(main_log_hits.empty()) << JoinLines(main_log_hits);
	EXPECT_TRUE(main_png_hits.empty()) << JoinLines(main_png_hits);
	EXPECT_TRUE(main_locale_host_hits.empty()) << JoinLines(main_locale_host_hits);
	EXPECT_FALSE(gui_log_hits.empty());
	EXPECT_FALSE(gui_png_hits.empty());
	EXPECT_FALSE(gui_locale_translation_hits.empty());
	EXPECT_FALSE(gui_locale_catalog_hits.empty());
	EXPECT_TRUE(locale_cpp_wx_hits.empty()) << JoinLines(locale_cpp_wx_hits);
	EXPECT_TRUE(headless_log_hits.empty()) << JoinLines(headless_log_hits);
	EXPECT_TRUE(headless_png_hits.empty()) << JoinLines(headless_png_hits);
	EXPECT_TRUE(headless_locale_hits.empty()) << JoinLines(headless_locale_hits);
	EXPECT_TRUE(headless_host_include_hits.empty()) << JoinLines(headless_host_include_hits);
	EXPECT_FALSE(std::filesystem::exists(legacy_headless_runtime_host_cpp));
}

TEST(host_boundary_policy, runtime_process_and_optional_facility_contracts_stay_split) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const app_runtime_h = root / "src" / "app_runtime.h";
	auto const app_runtime_cpp = root / "src" / "app_runtime.cpp";
	auto const app_runtime_facilities_cpp = root / "src" / "app_runtime_facilities.cpp";
	auto const gui_runtime_host_h = root / "src" / "gui_wx_runtime_host.h";
	auto const gui_runtime_host_cpp = root / "src" / "gui_wx_runtime_host.cpp";
	auto const runtime_process_host_h = root / "src" / "runtime_process_host.h";
	auto const runtime_optional_facility_host_h = root / "src" / "runtime_optional_facility_host.h";

	auto app_runtime_process_hook_hits = FindLiteralHits(app_runtime_cpp, "process_host.prime_process_logging");
	auto app_runtime_optional_png_hits = FindLiteralHits(app_runtime_cpp, "optional_facility_host.install_png_image_handler");
	auto app_runtime_legacy_png_hits = FindLiteralHits(app_runtime_cpp, "host_hooks.install_png_image_handler");
	auto facilities_process_hook_hits = FindLiteralHits(app_runtime_facilities_cpp, "process_host.prime_process_logging");
	auto facilities_optional_png_hits = FindLiteralHits(app_runtime_facilities_cpp, "optional_facility_host.install_png_image_handler");
	auto facilities_legacy_png_hits = FindLiteralHits(app_runtime_facilities_cpp, "host_hooks.install_png_image_handler");
	auto runtime_header_process_hits = FindLiteralHits(app_runtime_h, "RuntimeProcessHost process_host");
	auto runtime_header_optional_hits = FindLiteralHits(app_runtime_h, "RuntimeOptionalFacilityHost optional_facility_host");
	auto gui_runtime_header_process_hits = FindLiteralHits(gui_runtime_host_h, "runtime_process_host.h");
	auto gui_runtime_header_optional_hits = FindLiteralHits(gui_runtime_host_h, "runtime_optional_facility_host.h");
	auto gui_runtime_cpp_process_builder_hits = FindLiteralHits(gui_runtime_host_cpp, "BuildGuiWxRuntimeProcessHost()");
	auto gui_runtime_cpp_optional_builder_hits = FindLiteralHits(gui_runtime_host_cpp, "BuildGuiWxRuntimeOptionalFacilityHost()");
	auto main_process_builder_hits = FindLiteralHits(main_cpp, "BuildGuiWxRuntimeProcessHost()");
	auto main_optional_builder_hits = FindLiteralHits(main_cpp, "BuildGuiWxRuntimeOptionalFacilityHost()");
	auto process_header_wx_hits = FindWxMarkers(runtime_process_host_h);
	auto optional_header_wx_hits = FindWxMarkers(runtime_optional_facility_host_h);

	EXPECT_FALSE(app_runtime_process_hook_hits.empty());
	EXPECT_TRUE(app_runtime_optional_png_hits.empty()) << JoinLines(app_runtime_optional_png_hits);
	EXPECT_TRUE(app_runtime_legacy_png_hits.empty()) << JoinLines(app_runtime_legacy_png_hits);
	EXPECT_TRUE(facilities_process_hook_hits.empty()) << JoinLines(facilities_process_hook_hits);
	EXPECT_FALSE(facilities_optional_png_hits.empty());
	EXPECT_TRUE(facilities_legacy_png_hits.empty()) << JoinLines(facilities_legacy_png_hits);
	EXPECT_FALSE(runtime_header_process_hits.empty());
	EXPECT_FALSE(runtime_header_optional_hits.empty());
	EXPECT_FALSE(gui_runtime_header_process_hits.empty());
	EXPECT_FALSE(gui_runtime_header_optional_hits.empty());
	EXPECT_FALSE(gui_runtime_cpp_process_builder_hits.empty());
	EXPECT_FALSE(gui_runtime_cpp_optional_builder_hits.empty());
	EXPECT_TRUE(main_process_builder_hits.empty()) << JoinLines(main_process_builder_hits);
	EXPECT_TRUE(main_optional_builder_hits.empty()) << JoinLines(main_optional_builder_hits);
	EXPECT_TRUE(process_header_wx_hits.empty()) << JoinLines(process_header_wx_hits);
	EXPECT_TRUE(optional_header_wx_hits.empty()) << JoinLines(optional_header_wx_hits);
}

TEST(host_boundary_policy, headless_runtime_bootstrap_uses_minimal_runtime_init_options) {
	auto const root = ProjectRoot();
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";

	auto command_default_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "bool initialize_commands = false");
	auto command_assignment_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.initialize_commands = initialize_commands;");
	auto locale_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.initialize_ui_locale = false;");
	auto automation_factory_enabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.register_automation_script_factory = true;");
	auto font_warmup_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.warm_subtitles_provider_font_cache = false;");
	auto export_filters_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.register_export_filters = false;");
	auto png_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.install_png_handler = false;");

	EXPECT_FALSE(command_default_hits.empty());
	EXPECT_FALSE(command_assignment_hits.empty());
	EXPECT_FALSE(locale_disabled_hits.empty());
	EXPECT_FALSE(automation_factory_enabled_hits.empty());
	EXPECT_FALSE(font_warmup_disabled_hits.empty());
	EXPECT_FALSE(export_filters_disabled_hits.empty());
	EXPECT_FALSE(png_disabled_hits.empty());
}

TEST(host_boundary_policy, shared_exe_headless_entry_flows_directly_to_plain_process_host) {
	auto const root = ProjectRoot();
	auto const app_entry_cpp = root / "src" / "app_entry.cpp";
	auto const headless_process_entry_cpp = root / "src" / "headless_process_entry.cpp";
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";
	auto const wx_headless_process_host_cpp = root / "src" / "wx_headless_process_host.cpp";
	auto const wx_headless_process_host_h = root / "src" / "wx_headless_process_host.h";

	auto app_entry_initializer_hits = FindLiteralHits(app_entry_cpp, "wxInitializer");
	auto app_entry_plain_entry_include_hits = FindLiteralHits(app_entry_cpp, "headless_process_entry.h");
	auto app_entry_runtime_include_hits = FindLiteralHits(app_entry_cpp, "headless_runtime_bootstrap.h");
	auto app_entry_plan_parse_hits = FindLiteralHits(app_entry_cpp, "ParseAppLaunchPlan(args)");
	auto app_entry_plain_host_call_hits = FindLiteralHits(app_entry_cpp, "RunAppLaunchPlanInPlainProcessHost(launch_plan)");
	auto app_entry_global_input_hits = FindLiteralHits(app_entry_cpp, "SendInput");
	auto app_entry_return_key_hits = FindLiteralHits(app_entry_cpp, "VK_RETURN");
	auto process_entry_runtime_include_hits = FindLiteralHits(headless_process_entry_cpp, "headless_runtime_bootstrap.h");
	auto process_entry_runtime_call_hits = FindLiteralHits(headless_process_entry_cpp, "RunPlainProcessLaunchPlan(plan)");
	auto process_entry_wx_hits = FindWxMarkers(headless_process_entry_cpp);
	auto bootstrap_initializer_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxInitializer");

	EXPECT_TRUE(app_entry_initializer_hits.empty()) << JoinLines(app_entry_initializer_hits);
	EXPECT_FALSE(app_entry_plain_entry_include_hits.empty());
	EXPECT_TRUE(app_entry_runtime_include_hits.empty()) << JoinLines(app_entry_runtime_include_hits);
	EXPECT_FALSE(app_entry_plan_parse_hits.empty());
	EXPECT_FALSE(app_entry_plain_host_call_hits.empty());
	EXPECT_TRUE(app_entry_global_input_hits.empty()) << JoinLines(app_entry_global_input_hits);
	EXPECT_TRUE(app_entry_return_key_hits.empty()) << JoinLines(app_entry_return_key_hits);
	EXPECT_FALSE(process_entry_runtime_include_hits.empty());
	EXPECT_FALSE(process_entry_runtime_call_hits.empty());
	EXPECT_TRUE(process_entry_wx_hits.empty()) << JoinLines(process_entry_wx_hits);
	EXPECT_TRUE(bootstrap_initializer_hits.empty()) << JoinLines(bootstrap_initializer_hits);
	EXPECT_FALSE(std::filesystem::exists(wx_headless_process_host_cpp));
	EXPECT_FALSE(std::filesystem::exists(wx_headless_process_host_h));
}

TEST(host_boundary_policy, shared_process_config_globals_live_outside_gui_main) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const app_process_config_cpp = root / "src" / "app_process_config.cpp";

	auto main_config_namespace_hits = FindLiteralHits(main_cpp, "namespace config {");
	auto main_opt_hits = FindLiteralHits(main_cpp, "agi::Options *opt = nullptr;");
	auto main_mru_hits = FindLiteralHits(main_cpp, "agi::MRUManager *mru = nullptr;");
	auto main_path_hits = FindLiteralHits(main_cpp, "agi::Path *path = nullptr;");
	auto main_scripts_hits = FindLiteralHits(main_cpp, "Automation4::AutoloadScriptManager *global_scripts");

	auto config_namespace_hits = FindLiteralHits(app_process_config_cpp, "namespace config {");
	auto config_opt_hits = FindLiteralHits(app_process_config_cpp, "agi::Options *opt = nullptr;");
	auto config_mru_hits = FindLiteralHits(app_process_config_cpp, "agi::MRUManager *mru = nullptr;");
	auto config_path_hits = FindLiteralHits(app_process_config_cpp, "agi::Path *path = nullptr;");
	auto config_scripts_hits = FindLiteralHits(app_process_config_cpp, "Automation4::AutoloadScriptManager *global_scripts = nullptr;");

	EXPECT_TRUE(main_config_namespace_hits.empty()) << JoinLines(main_config_namespace_hits);
	EXPECT_TRUE(main_opt_hits.empty()) << JoinLines(main_opt_hits);
	EXPECT_TRUE(main_mru_hits.empty()) << JoinLines(main_mru_hits);
	EXPECT_TRUE(main_path_hits.empty()) << JoinLines(main_path_hits);
	EXPECT_TRUE(main_scripts_hits.empty()) << JoinLines(main_scripts_hits);

	EXPECT_FALSE(config_namespace_hits.empty());
	EXPECT_FALSE(config_opt_hits.empty());
	EXPECT_FALSE(config_mru_hits.empty());
	EXPECT_FALSE(config_path_hits.empty());
	EXPECT_FALSE(config_scripts_hits.empty());
}

TEST(host_boundary_policy, shared_headless_entry_bootstrap_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_entry_sources = {
		"src/app_entry.cpp",
	};
	std::set<std::string> const expected_headless_entry_bootstrap_entries = {
		"${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES}",
		"${AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES}",
		"src/app_launch_plan.cpp",
		"src/automation_command_executor.cpp",
		"src/automation_process_supervisor.cpp",
		"src/automation_scenario.cpp",
		"src/automation_scenario_runner.cpp",
		"src/fontcollector_cli_encoding.cpp",
		"src/headless_service_output.cpp",
		"src/headless_process_entry.cpp",
		"src/headless_runtime_bootstrap.cpp",
	};

	auto entry_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_EXE_ENTRY_SOURCES");
	auto headless_entry_bootstrap_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_HEADLESS_ENTRY_BOOTSTRAP_SOURCES");
	auto entry_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_EXE_ENTRY_SOURCES}");
	auto runtime_common_init_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES}");
	auto headless_entry_bootstrap_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_HEADLESS_ENTRY_BOOTSTRAP_SOURCES}");

	EXPECT_EQ(expected_entry_sources, entry_sources);
	EXPECT_EQ(expected_headless_entry_bootstrap_entries, headless_entry_bootstrap_sources);
	EXPECT_FALSE(entry_expansion_hits.empty());
	EXPECT_FALSE(runtime_common_init_expansion_hits.empty());
	EXPECT_FALSE(headless_entry_bootstrap_expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_common_init_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES}",
		"src/app_process_config.cpp",
		"src/app_runtime.cpp",
		"src/automation_runtime_profile.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES");
	auto support_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(support_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_common_init_support_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"${AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES}",
		"src/app_runtime_init.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES");
	auto optional_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(optional_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_optional_facility_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/app_runtime_facilities.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_locale_core_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"${AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES}",
		"src/aegisublocale.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES");
	auto locale_support_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(locale_support_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_locale_support_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/locale_pick.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, gui_runtime_wx_host_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/gui_wx_dispatch_event.cpp",
		"src/gui_wx_locale_host.cpp",
		"src/gui_wx_runtime_entry_host.cpp",
		"src/gui_wx_runtime_host.cpp",
		"src/gui_wx_subtitle_format_host.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_GUI_RUNTIME_WX_HOST_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_GUI_RUNTIME_WX_HOST_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, gui_bootstrap_wx_host_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/gui_wx_bootstrap_ui_host.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_GUI_BOOTSTRAP_WX_HOST_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_GUI_BOOTSTRAP_WX_HOST_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_selection_request_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/charset_choice.cpp",
		"src/locale_choice.cpp",
		"src/single_choice_layout.cpp",
		"src/subtitle_fps_choice.cpp",
		"src/track_choice.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_SELECTION_REQUEST_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_SELECTION_REQUEST_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_cli_inspect_service_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/ass_info_service.cpp",
		"src/media_inspect_service.cpp",
		"src/trace_inspect_service.cpp",
		"src/trace_summary_service.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, app_launch_plan_owns_cli11_parsing) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto const app_launch_plan_cpp = root / "src" / "app_launch_plan.cpp";

	auto cli11_include_hits = FindLiteralHits(app_launch_plan_cpp, "CLI/CLI.hpp");
	auto cli11_package_hits = FindLiteralHits(cmake_lists, "find_package(CLI11 CONFIG REQUIRED)");
	auto legacy_probe_parser_hits = FindLiteralHits(cmake_lists, "src/headless_playback_probe.cpp");

	EXPECT_FALSE(cli11_include_hits.empty());
	EXPECT_FALSE(cli11_package_hits.empty());
	EXPECT_TRUE(legacy_probe_parser_hits.empty()) << JoinLines(legacy_probe_parser_hits);
}

TEST(host_boundary_policy, shared_headless_bootstrap_depends_on_typed_service_headers) {
	auto const root = ProjectRoot();
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";
	auto bootstrap_service_cli_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_service_cli.h");
	auto bootstrap_service_output_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_service_output.h");
	auto legacy_cli_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_cli");

	EXPECT_FALSE(bootstrap_service_cli_hits.empty());
	EXPECT_FALSE(bootstrap_service_output_hits.empty());
	EXPECT_TRUE(legacy_cli_hits.empty()) << JoinLines(legacy_cli_hits);
}

TEST(host_boundary_policy, shared_playback_project_session_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_host_sources = {
		"src/headless_playback_session_host.cpp",
		"src/playback_probe_timer.cpp",
		"src/playback_session_timer.cpp",
		"src/project_query_service.cpp",
	};
	std::set<std::string> const expected_support_sources = {
		"${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_HOST_SOURCES}",
		"${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}",
	};
	std::set<std::string> const expected_core_sources = {
		"src/automation_session_service.cpp",
		"src/playback_probe_service.cpp",
		"src/playback_session_service.cpp",
		"src/project_session_service.cpp",
	};
	std::set<std::string> const expected_aggregate_entries = {
		"${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES}",
		"${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES}",
	};

	auto host_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_HOST_SOURCES");
	auto support_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES");
	auto project_media_open_query_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}");
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES");
	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SOURCES");
	auto host_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_HOST_SOURCES}");
	auto support_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES}");
	auto core_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES}");

	EXPECT_EQ(expected_host_sources, host_sources);
	EXPECT_EQ(expected_support_sources, support_sources);
	EXPECT_EQ(expected_core_sources, core_sources);
	EXPECT_EQ(expected_aggregate_entries, sources);
	EXPECT_FALSE(project_media_open_query_expansion_hits.empty());
	EXPECT_FALSE(host_expansion_hits.empty());
	EXPECT_FALSE(support_expansion_hits.empty());
	EXPECT_FALSE(core_expansion_hits.empty());
}

TEST(host_boundary_policy, shared_project_media_open_query_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/playback_query_service.cpp",
		"src/project_open_service.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, aegisub_core_sources_keep_host_coupled_clusters_out) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::set<std::string> const expected_core_sources = {
		"src/app_process_config.cpp",
		"src/ass_io_core.cpp",
		"src/ass_fixstyle_core.cpp",
		"src/async_video_provider.cpp",
		"src/async_video_provider_host.cpp",
		"src/async_video_trace.cpp",
		"src/audio_provider_factory.cpp",
		"src/auto4_lua.cpp",
		"src/auto4_lua_assfile.cpp",
		"src/auto4_lua_dialog.cpp",
		"src/auto4_lua_progresssink.cpp",
		"src/automation/automation_lua_debug_backend.cpp",
		"src/automation/automation_features.cpp",
		"src/automation/automation_script_collection.cpp",
		"src/cache_cleanup.cpp",
		"src/context_core_session.cpp",
		"src/ebu_export_settings.cpp",
		"src/export_framerate_transform.cpp",
		"src/numeric_utils.cpp",
		"src/presentation/subtitle_grid_display.cpp",
		"src/presentation/subtitle_grid_diff.cpp",
		"src/presentation/subtitle_grid_projection.cpp",
		"src/presentation/subtitle_grid_query_service.cpp",
		"src/project_session_ops.cpp",
		"src/provider_index_cache.cpp",
		"src/provider_catalog_builder.cpp",
		"src/provider_open_policy.cpp",
		"src/resample_dialog_policy.cpp",
		"src/resolution_resampler.cpp",
		"src/subtitle_format.cpp",
		"src/subtitle_format_ass.cpp",
		"src/subtitle_format_ebu3264.cpp",
		"src/subtitle_format_encore.cpp",
		"src/subtitle_format_microdvd.cpp",
		"src/subtitle_format_mkv.cpp",
		"src/subtitle_format_srt.cpp",
		"src/subtitle_format_ssa.cpp",
		"src/subtitle_format_transtation.cpp",
		"src/subtitle_format_ttxt.cpp",
		"src/subtitle_format_txt.cpp",
		"src/subtitle_overlay_blend.cpp",
		"src/subtitles_provider.cpp",
		"src/subtitles_provider_libass.cpp",
		"src/libass_runtime.cpp",
		"src/secondary_subtitle_decoder.cpp",
		"src/video_provider_manager.cpp",
		"src/video_provider_cache.cpp",
		"src/video_provider_dummy.cpp",
		"src/video_provider_yuv4mpeg.cpp",
		"src/video_property_update.cpp",
		"src/video_property_update_policy.cpp",
		"src/video_property_update_requests.cpp",
		"src/video_snapshot_ops.cpp",
		"src/video_session_core_ops.cpp",
		"src/video_session_ops.cpp",
		"${AEGISUB_SHARED_SELECTION_REQUEST_SOURCES}",
		"src/ui_services.cpp",
	};
	std::set<std::string> const host_coupled_sources = {
		"${AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES}",
		"${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES}",
		"${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}",
		"src/automation/automation_live_host.cpp",
		"src/auto4_base.cpp",
		"src/wx_automation_dialog.cpp",
		"src/wx_automation_runtime.cpp",
		"src/context.cpp",
		"src/search_replace_engine.cpp",
		"src/subs_controller.cpp",
		"src/video_controller.cpp",
	};

	for (auto const& source : expected_core_sources)
		EXPECT_NE(core_sources.end(), core_sources.find(source)) << source;
	for (auto const& source : host_coupled_sources)
		EXPECT_EQ(core_sources.end(), core_sources.find(source)) << source;
}

TEST(host_boundary_policy, display_renderer_implementations_stay_out_of_aegisub_core) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto const core_c_api_config_template = root / "cmake" / "AegisubCoreCAPIConfig.cmake.in";

	auto core_target_sources = ReadCMakeTargetSourceEntries(cmake_lists, "aegisub_core");
	auto app_target_sources = ReadCMakeTargetSourceEntries(cmake_lists, "Aegisub");

	EXPECT_NE(core_target_sources.end(), core_target_sources.find("src/video_renderer_placebo_runtime.cpp"));
	EXPECT_EQ(core_target_sources.end(), core_target_sources.find("src/video_renderer_placebo_gl.cpp"));
	EXPECT_NE(app_target_sources.end(), app_target_sources.find("src/video_renderer_placebo_gl.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_link_libraries(aegisub_features INTERFACE OpenGL::GL)").empty());
	EXPECT_TRUE(FindLiteralHits(core_c_api_config_template, "find_dependency(OpenGL").empty());
}

TEST(host_boundary_policy, core_links_core_feature_target_not_gui_feature_facade) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_api_static_export_targets = ReadNamedCMakeSetEntries(
		cmake_lists, "AEGISUB_CORE_API_STATIC_EXPORT_TARGETS");

	EXPECT_FALSE(FindLiteralHits(cmake_lists, "add_library(aegisub_core_features INTERFACE)").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "target_link_libraries(aegisub_features INTERFACE aegisub_core_features)").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "target_link_libraries(aegisub_core PRIVATE aegisub_core_features").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_link_libraries(aegisub_core PRIVATE aegisub_features").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "aegisub_core_features").empty());
	EXPECT_NE(core_api_static_export_targets.end(),
		core_api_static_export_targets.find("aegisub_core_features"));
	EXPECT_EQ(core_api_static_export_targets.end(),
		core_api_static_export_targets.find("aegisub_features"));
	EXPECT_FALSE(FindLiteralHits(cmake_lists,
		"install(TARGETS ${AEGISUB_CORE_API_STATIC_EXPORT_TARGETS}").empty());
}

TEST(host_boundary_policy, platform_font_listers_are_core_owned_not_recompiled_by_gui) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	auto core_target_sources = ReadCMakeTargetSourceEntries(cmake_lists, "aegisub_core");
	auto app_target_sources = ReadCMakeTargetSourceEntries(cmake_lists, "Aegisub");

	EXPECT_NE(core_sources.end(), core_sources.find("src/font_file_lister.cpp"));
	for (auto const& source : {
		"src/font_file_lister_gdi.cpp",
		"src/font_file_lister_dwrite.cpp",
		"src/font_file_lister_coretext.mm",
		"src/font_file_lister_fontconfig.cpp",
	}) {
		EXPECT_NE(core_target_sources.end(), core_target_sources.find(source)) << source;
		EXPECT_EQ(app_target_sources.end(), app_target_sources.find(source)) << source;
	}
}

TEST(host_boundary_policy, ui_timer_core_contract_is_not_recompiled_by_gui) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	auto app_target_sources = ReadCMakeTargetSourceEntries(cmake_lists, "Aegisub");

	for (auto const& source : {
		"src/ui_timer.cpp",
		"src/threaded_ui_timer.cpp",
	}) {
		EXPECT_NE(core_sources.end(), core_sources.find(source)) << source;
		EXPECT_EQ(app_target_sources.end(), app_target_sources.find(source)) << source;
	}
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "${AEGISUB_GUI_RUNTIME_WX_HOST_SOURCES}").empty());
}

TEST(host_boundary_policy, aegisub_core_sources_do_not_include_wx_format_adapter) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");

	for (auto const& source : core_sources) {
		if (!StartsWith(source, "src/"))
			continue;

		auto const path = root / std::filesystem::path(source);
		if (!std::filesystem::exists(path))
			continue;

		auto hits = FindLiteralHits(path, "#include \"format.h\"");
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, aegisub_core_sources_stay_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");

	for (auto const& source : core_sources) {
		if (!StartsWith(source, "src/"))
			continue;

		auto const path = root / std::filesystem::path(source);
		if (!std::filesystem::exists(path))
			continue;

		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, aegisub_core_same_name_headers_stay_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");

	for (auto const& source : core_sources) {
		if (!StartsWith(source, "src/") || !EndsWith(source, ".cpp"))
			continue;

		auto header = root / std::filesystem::path(source);
		header.replace_extension(".h");
		if (!std::filesystem::exists(header))
			continue;

		auto hits = FindWxMarkers(header);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, aegisub_core_src_local_include_closure_stays_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	auto closure = BuildSrcLocalIncludeClosure(root, core_sources);

	ASSERT_FALSE(closure.empty());

	for (auto const& path : closure) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, core_api_facade_and_c_wrapper_are_core_owned_and_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto const core_c_api_config_template = root / "cmake" / "AegisubCoreCAPIConfig.cmake.in";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	auto c_api_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_C_API_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "core_api_facade.h",
		root / "src" / "core_api_facade.cpp",
		root / "src" / "include" / "aegisub" / "core_c_api.h",
		root / "src" / "core_c_api.cpp",
	};

	EXPECT_NE(core_sources.end(), core_sources.find("src/core_api_facade.cpp"));
	EXPECT_NE(c_api_sources.end(), c_api_sources.find("src/core_c_api.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "add_library(aegisub_core_c_api_static STATIC").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "add_library(aegisub_core_c_api SHARED").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "set_target_properties(luajit libaegisub aegisub_core aegisub_core_c_api_static PROPERTIES").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "POSITION_INDEPENDENT_CODE ON").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "C_VISIBILITY_PRESET hidden").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "CXX_VISIBILITY_PRESET hidden").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "VISIBILITY_INLINES_HIDDEN ON").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "src/aegisub_core_c_api.version").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "src/aegisub_core_c_api.exports").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "set(AEGISUB_CORE_API_SHARED_EXPORT_TARGETS").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "set(AEGISUB_CORE_API_STATIC_EXPORT_TARGETS").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "install(TARGETS ${AEGISUB_CORE_API_SHARED_EXPORT_TARGETS}").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "install(TARGETS ${AEGISUB_CORE_API_STATIC_EXPORT_TARGETS}").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "install(EXPORT AegisubCoreCAPITargets").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "install(EXPORT AegisubCoreCAPISharedTargets").empty() == false);
	EXPECT_FALSE(FindLiteralHits(core_c_api_config_template,
		"include(\"${CMAKE_CURRENT_LIST_DIR}/AegisubCoreCAPISharedTargets.cmake\")").empty());
	EXPECT_FALSE(FindLiteralHits(core_c_api_config_template,
		"include(\"${CMAKE_CURRENT_LIST_DIR}/AegisubCoreCAPITargets.cmake\")").empty());
	EXPECT_FALSE(FindLiteralHits(core_c_api_config_template,
		"IN_LIST AegisubCoreCAPI_FIND_COMPONENTS").empty());
	EXPECT_FALSE(FindLiteralHits(core_c_api_config_template,
		"find_dependency(Fontconfig REQUIRED)").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "RUNTIME DESTINATION bin").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "LIBRARY DESTINATION lib").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "ARCHIVE DESTINATION lib").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists,
		"install(FILES src/include/aegisub/core_c_api.h").empty() == false);
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/core_api_facade.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/core_c_api.cpp").empty());

	auto const core_api_facade_cpp = root / "src" / "core_api_facade.cpp";
	for (auto const& path : expected_wx_free_paths) {
		ASSERT_TRUE(std::filesystem::exists(path)) << path;
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
	EXPECT_TRUE(FindLiteralHits(core_api_facade_cpp, "OPT_GET(").empty());
	EXPECT_TRUE(FindLiteralHits(core_api_facade_cpp, "OPT_SET(").empty());
	EXPECT_TRUE(FindLiteralHits(core_api_facade_cpp, "#include \"options.h\"").empty());
	EXPECT_FALSE(FindLiteralHits(core_api_facade_cpp, "WriteAssFileForCore(").empty());
	EXPECT_FALSE(FindLiteralHits(core_api_facade_cpp, "AssWriteOptions write_options").empty());
	EXPECT_TRUE(FindLiteralHits(core_api_facade_cpp, "AssSubtitleFormat").empty());

	auto const elf_exports = root / "src" / "aegisub_core_c_api.version";
	auto const macos_exports = root / "src" / "aegisub_core_c_api.exports";
	ASSERT_TRUE(std::filesystem::exists(elf_exports));
	ASSERT_TRUE(std::filesystem::exists(macos_exports));
	auto const exported_symbols = ReadCoreCApiExportedFunctionNames(root / "src" / "include" / "aegisub" / "core_c_api.h");
	ASSERT_FALSE(exported_symbols.empty());
	for (auto const& symbol : exported_symbols) {
		EXPECT_TRUE(FindLiteralHits(elf_exports, symbol).empty() == false) << symbol;
		EXPECT_TRUE(FindLiteralHits(macos_exports, std::string("_") + symbol).empty() == false) << symbol;
	}
	EXPECT_TRUE(FindLiteralHits(elf_exports, "local:").empty() == false);
}

TEST(host_boundary_policy, concrete_subtitle_provider_cluster_is_core_owned_and_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_provider_paths = {
		root / "src" / "include" / "aegisub" / "subtitles_provider.h",
		root / "src" / "subtitles_provider.cpp",
		root / "src" / "subtitles_provider_libass.cpp",
		root / "src" / "libass_runtime.h",
		root / "src" / "libass_runtime.cpp",
		root / "src" / "secondary_subtitle_decoder.cpp",
		root / "src" / "provider_catalog_builder.h",
		root / "src" / "provider_catalog_builder.cpp",
		root / "src" / "provider_factory_entry.h",
		root / "src" / "provider_open_policy.h",
		root / "src" / "provider_open_policy.cpp",
	};

	for (auto const& source : {
		"src/subtitles_provider.cpp",
		"src/subtitles_provider_libass.cpp",
		"src/libass_runtime.cpp",
		"src/secondary_subtitle_decoder.cpp",
		"src/provider_catalog_builder.cpp",
		"src/provider_open_policy.cpp",
	}) {
		EXPECT_NE(core_sources.end(), core_sources.find(source)) << source;
	}

	for (auto const& path : expected_provider_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}

	EXPECT_FALSE(FindLiteralHits(cmake_lists, "Provider dependency audit:").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "core-owned now: subtitles_provider.cpp/libass/secondary decoder").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "core-owned now: audio/video provider managers").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "host-owned for now: playback/controllers/render display pieces").empty());
}

TEST(host_boundary_policy, project_session_ops_is_core_owned_and_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "media_open_contract.h",
		root / "src" / "project_session_ops.h",
		root / "src" / "project_session_ops.cpp",
	};

	EXPECT_NE(core_sources.end(), core_sources.find("src/project_session_ops.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/project_session_ops.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "project_session_ops.cpp", "#include \"format.h\"").empty());
	EXPECT_FALSE(FindLiteralHits(root / "src" / "project_session_ops.cpp", "#include \"translation_service.h\"").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, ass_fixstyle_core_keeps_async_video_provider_off_export_filter_wx_surface) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "ass_fixstyle_core.h",
		root / "src" / "ass_fixstyle_core.cpp",
	};

	EXPECT_NE(core_sources.end(), core_sources.find("src/ass_fixstyle_core.cpp"));
	EXPECT_TRUE(FindLiteralHits(root / "src" / "async_video_provider.cpp", "#include \"export_fixstyle.h\"").empty());
	EXPECT_FALSE(FindLiteralHits(root / "src" / "async_video_provider.cpp", "#include \"ass_fixstyle_core.h\"").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, async_video_provider_is_core_owned_and_traces_through_wx_free_seam) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "async_video_provider.h",
		root / "src" / "async_video_provider.cpp",
		root / "src" / "async_video_provider_host.h",
		root / "src" / "async_video_provider_host.cpp",
		root / "src" / "async_video_trace.h",
		root / "src" / "async_video_trace.cpp",
	};

	EXPECT_NE(core_sources.end(), core_sources.find("src/async_video_provider.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/async_video_provider_host.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/async_video_trace.cpp"));
	EXPECT_TRUE(FindLiteralHits(root / "src" / "async_video_provider.cpp", "#include \"perf_trace.h\"").empty());
	EXPECT_FALSE(FindLiteralHits(root / "src" / "async_video_provider.cpp", "#include \"async_video_trace.h\"").empty());
	EXPECT_FALSE(FindLiteralHits(root / "src" / "perf_trace.cpp", "async_video_trace::SetSink").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, video_session_ops_are_core_owned_while_ui_playback_remains_host_owned) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "media_open_contract.h",
		root / "src" / "video_session_core_ops.h",
		root / "src" / "video_session_core_ops.cpp",
		root / "src" / "video_session_ops.h",
		root / "src" / "video_session_ops.cpp",
	};

	EXPECT_NE(core_sources.end(), core_sources.find("src/video_session_core_ops.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/video_session_ops.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_session_ops.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_session_core_ops.cpp").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, video_snapshot_ops_are_core_owned_and_use_libaegisub_format) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "video_snapshot_ops.h",
		root / "src" / "video_snapshot_ops.cpp",
	};
	auto const snapshot_ops_cpp = root / "src" / "video_snapshot_ops.cpp";

	EXPECT_NE(core_sources.end(), core_sources.find("src/video_snapshot_ops.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_snapshot_ops.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(snapshot_ops_cpp, "#include \"format.h\"").empty());
	EXPECT_FALSE(FindLiteralHits(snapshot_ops_cpp, "#include <libaegisub/format.h>").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, video_property_update_plan_and_apply_are_core_owned) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "video_property_update.h",
		root / "src" / "video_property_update.cpp",
		root / "src" / "video_property_update_policy.cpp",
		root / "src" / "video_property_update_requests.h",
		root / "src" / "video_property_update_requests.cpp",
	};
	auto const dialog_video_properties_cpp = root / "src" / "dialog_video_properties.cpp";

	EXPECT_NE(core_sources.end(), core_sources.find("src/video_property_update.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/video_property_update_policy.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/video_property_update_requests.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_property_update.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_property_update_policy.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_property_update_requests.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_video_properties_cpp, "#include \"format.h\"").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_video_properties_cpp, "#include \"compat.h\"").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_video_properties_cpp, "#include \"async_video_provider.h\"").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_video_properties_cpp, "#include <wx/intl.h>").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_video_properties_cpp, "ShouldSetVideoProperties").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_video_properties_cpp, "GetColorSpace").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_video_properties_cpp, "BuildVideoPropertyUpdateInput").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_video_properties_cpp, "BuildVideoResolutionMismatchRequest").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_video_properties_cpp, "BuildLayoutResRequest").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, resolution_resampler_is_core_owned_and_uses_translation_service) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto const resampler_cpp = root / "src" / "resolution_resampler.cpp";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");

	EXPECT_NE(core_sources.end(), core_sources.find("src/resolution_resampler.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/resolution_resampler.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(resampler_cpp, "#include \"compat.h\"").empty());
	EXPECT_TRUE(FindLiteralHits(resampler_cpp, "#include \"utils.h\"").empty());
	EXPECT_TRUE(FindLiteralHits(resampler_cpp, "#include <wx/intl.h>").empty());
	EXPECT_FALSE(FindLiteralHits(resampler_cpp, "#include \"translation_service.h\"").empty());

	auto hits = FindWxMarkers(resampler_cpp);
	EXPECT_TRUE(hits.empty()) << JoinLines(hits);
}

TEST(host_boundary_policy, resample_dialog_policy_is_core_owned_while_wx_dialog_only_binds_controls) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "resample_dialog_policy.h",
		root / "src" / "resample_dialog_policy.cpp",
	};
	auto const dialog_resample_cpp = root / "src" / "dialog_resample.cpp";

	EXPECT_NE(core_sources.end(), core_sources.find("src/resample_dialog_policy.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/resample_dialog_policy.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_resample_cpp, "file_has_layoutres_sensitive_tags").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_resample_cpp, "BuildResampleDialogReference").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_resample_cpp, "InitializeResampleSettings").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_resample_cpp, "BuildResampleDialogButtonState").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_resample_cpp, "HasLayoutResSensitiveTags").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, paste_over_policy_is_core_owned_while_wx_dialog_only_binds_controls) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "paste_over_policy.h",
		root / "src" / "paste_over_policy.cpp",
	};
	auto const dialog_paste_over_cpp = root / "src" / "dialog_paste_over.cpp";

	for (auto const& path : expected_wx_free_paths)
		ASSERT_TRUE(std::filesystem::exists(path)) << path;
	ASSERT_TRUE(std::filesystem::exists(dialog_paste_over_cpp));

	EXPECT_NE(core_sources.end(), core_sources.find("src/paste_over_policy.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/paste_over_policy.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_paste_over_cpp, "paste_over_policy.h").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_paste_over_cpp, "NormalizeFields").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_paste_over_cpp, "BuildAllFields").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_paste_over_cpp, "BuildTimesFields").empty());
	EXPECT_FALSE(FindLiteralHits(dialog_paste_over_cpp, "BuildTextFields").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_paste_over_cpp, "normalize_paste_over_options").empty());
	EXPECT_TRUE(FindLiteralHits(dialog_paste_over_cpp, "PASTE_OVER_FIELD_COUNT").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, shared_selection_request_helpers_keep_wx_at_single_choice_adapter_edge) {
	auto const root = ProjectRoot();
	auto const charset_choice_cpp = root / "src" / "charset_choice.cpp";
	auto const locale_choice_cpp = root / "src" / "locale_choice.cpp";
	auto const subtitle_fps_choice_cpp = root / "src" / "subtitle_fps_choice.cpp";
	auto const track_choice_cpp = root / "src" / "track_choice.cpp";
	auto const wx_single_choice_dialog_h = root / "src" / "wx_single_choice_dialog.h";

	auto charset_choice_wx_hits = FindWxMarkers(charset_choice_cpp);
	auto locale_choice_wx_hits = FindWxMarkers(locale_choice_cpp);
	auto subtitle_fps_choice_wx_hits = FindWxMarkers(subtitle_fps_choice_cpp);
	auto track_choice_wx_hits = FindWxMarkers(track_choice_cpp);
	auto charset_choice_request_hits = FindLiteralHits(charset_choice_cpp, "charset_choice.detected_charsets");
	auto locale_choice_request_hits = FindLiteralHits(locale_choice_cpp, "locale_choice.ui_language");
	auto subtitle_fps_choice_request_hits = FindLiteralHits(subtitle_fps_choice_cpp, "subtitle_fps_choice.selection");
	auto adapter_charset_request_hits = FindLiteralHits(wx_single_choice_dialog_h, "charset_choice.detected_charsets");
	auto adapter_locale_choice_request_hits = FindLiteralHits(wx_single_choice_dialog_h, "locale_choice.ui_language");
	auto adapter_subtitle_fps_request_hits = FindLiteralHits(wx_single_choice_dialog_h, "subtitle_fps_choice.selection");
	auto adapter_track_audio_hits = FindLiteralHits(wx_single_choice_dialog_h, "track_choice.audio");
	auto adapter_track_subtitle_hits = FindLiteralHits(wx_single_choice_dialog_h, "track_choice.subtitle");
	auto adapter_track_video_hits = FindLiteralHits(wx_single_choice_dialog_h, "track_choice.video");

	EXPECT_TRUE(charset_choice_wx_hits.empty()) << JoinLines(charset_choice_wx_hits);
	EXPECT_TRUE(locale_choice_wx_hits.empty()) << JoinLines(locale_choice_wx_hits);
	EXPECT_TRUE(subtitle_fps_choice_wx_hits.empty()) << JoinLines(subtitle_fps_choice_wx_hits);
	EXPECT_TRUE(track_choice_wx_hits.empty()) << JoinLines(track_choice_wx_hits);
	EXPECT_FALSE(charset_choice_request_hits.empty());
	EXPECT_FALSE(locale_choice_request_hits.empty());
	EXPECT_FALSE(subtitle_fps_choice_request_hits.empty());
	EXPECT_FALSE(adapter_charset_request_hits.empty());
	EXPECT_FALSE(adapter_locale_choice_request_hits.empty());
	EXPECT_FALSE(adapter_subtitle_fps_request_hits.empty());
	EXPECT_FALSE(adapter_track_audio_hits.empty());
	EXPECT_FALSE(adapter_track_subtitle_hits.empty());
	EXPECT_FALSE(adapter_track_video_hits.empty());
}

TEST(host_boundary_policy, shared_provider_track_selection_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/ffmpegsource_common.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PROVIDER_TRACK_SELECTION_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PROVIDER_TRACK_SELECTION_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, ffms_provider_cluster_is_core_owned_and_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "audio_provider_ffmpegsource.cpp",
		root / "src" / "video_provider_ffmpegsource.cpp",
		root / "src" / "ffmpegsource_common.cpp",
		root / "src" / "ffmpegsource_common.h",
		root / "src" / "cache_cleanup.h",
		root / "src" / "cache_cleanup.cpp",
		root / "src" / "provider_index_cache.h",
		root / "src" / "provider_index_cache.cpp",
	};

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}

	EXPECT_FALSE(FindLiteralHits(cmake_lists, "target_sources(aegisub_core PRIVATE").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/audio_provider_ffmpegsource.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/video_provider_ffmpegsource.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/audio_provider_ffmpegsource.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_provider_ffmpegsource.cpp").empty());
}

TEST(host_boundary_policy, lsmas_provider_cluster_is_core_owned_and_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "audio_provider_lsmasnative.cpp",
		root / "src" / "video_provider_lsmasnative.cpp",
		root / "src" / "lsmas_native_runtime.cpp",
		root / "src" / "lsmas_native_api.h",
		root / "src" / "lsmas_provider_common.h",
		root / "src" / "lsmas_provider_common.cpp",
		root / "src" / "scenechange_native_runtime.cpp",
		root / "src" / "scenechange_native_api.h",
	};

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}

	EXPECT_FALSE(FindLiteralHits(cmake_lists, "target_sources(aegisub_core PRIVATE").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/audio_provider_lsmasnative.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/video_provider_lsmasnative.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/scenechange_native_runtime.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/audio_provider_lsmasnative.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_provider_lsmasnative.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/scenechange_native_runtime.cpp").empty());
}

TEST(host_boundary_policy, builtin_video_provider_cluster_is_core_owned_and_wx_free) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "video_provider_dummy.h",
		root / "src" / "video_provider_cache.cpp",
		root / "src" / "video_provider_dummy.cpp",
		root / "src" / "video_provider_yuv4mpeg.cpp",
	};

	EXPECT_NE(core_sources.end(), core_sources.find("src/video_provider_cache.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/video_provider_dummy.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/video_provider_yuv4mpeg.cpp"));
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_provider_cache.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_provider_dummy.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_provider_yuv4mpeg.cpp").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, provider_managers_are_core_owned_and_register_host_providers_at_edge) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_CORE_SOURCES");
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "audio_provider_factory.h",
		root / "src" / "audio_provider_factory.cpp",
		root / "src" / "audio_provider_avs.cpp",
		root / "src" / "avisynth_legacy_path.h",
		root / "src" / "avisynth_legacy_path.cpp",
		root / "src" / "avisynth_provider_registration.h",
		root / "src" / "avisynth_runtime_policy.h",
		root / "src" / "avisynth_wrap.h",
		root / "src" / "avisynth_wrap.cpp",
		root / "src" / "video_provider_avs.cpp",
		root / "src" / "video_provider_manager.h",
		root / "src" / "video_provider_manager.cpp",
	};

	EXPECT_NE(core_sources.end(), core_sources.find("src/audio_provider_factory.cpp"));
	EXPECT_NE(core_sources.end(), core_sources.find("src/video_provider_manager.cpp"));
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "target_sources(aegisub_core PRIVATE").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/audio_provider_avs.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/avisynth_legacy_path.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/avisynth_wrap.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(cmake_lists, "src/video_provider_avs.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/audio_provider_factory.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_provider_manager.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/audio_provider_avs.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/avisynth_legacy_path.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/avisynth_wrap.cpp").empty());
	EXPECT_TRUE(FindLiteralHits(cmake_lists, "target_sources(Aegisub PRIVATE src/video_provider_avs.cpp").empty());
	EXPECT_FALSE(FindLiteralHits(root / "src" / "audio_provider_avs.cpp", "RegisterAudioProviderFactory").empty());
	EXPECT_FALSE(FindLiteralHits(root / "src" / "video_provider_avs.cpp", "RegisterVideoProviderFactory").empty());
	EXPECT_FALSE(FindLiteralHits(root / "src" / "main.cpp", "RegisterAvisynthProviderFactories();").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "main.cpp", "FreezeProviderFactoryRegistries();").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "main.cpp", "FinalizeProviderFactoryRegistries();").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "audio_provider_avs.cpp", "AvisynthAudioProviderRegistration").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "audio_provider_avs.cpp", "avisynth_audio_provider_registration").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "video_provider_avs.cpp", "AvisynthVideoProviderRegistration").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "video_provider_avs.cpp", "avisynth_video_provider_registration").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "audio_provider_factory.cpp", "avisynth_wrap.h").empty());
	EXPECT_TRUE(FindLiteralHits(root / "src" / "video_provider_manager.cpp", "avisynth_wrap.h").empty());

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}
}

TEST(host_boundary_policy, host_thread_and_registry_lifecycle_boundaries_stay_out_of_media_hot_paths) {
	auto const root = ProjectRoot();
	std::vector<std::filesystem::path> const hot_paths = {
		root / "src" / "async_video_provider.cpp",
		root / "src" / "audio_display_source.cpp",
		root / "src" / "audio_renderer.cpp",
		root / "src" / "audio_renderer_spectrum.cpp",
		root / "src" / "audio_renderer_waveform.cpp",
		root / "src" / "subtitles_provider_csri.cpp",
		root / "src" / "subtitles_provider_libass.cpp",
		root / "src" / "video_display.cpp",
		root / "src" / "video_provider_cache.cpp",
		root / "src" / "video_provider_dummy.cpp",
		root / "src" / "video_provider_ffmpegsource.cpp",
		root / "src" / "video_provider_lsmasnative.cpp",
		root / "src" / "video_provider_yuv4mpeg.cpp",
		root / "src" / "video_renderer_opengl.cpp",
		root / "src" / "video_renderer_placebo_gl.cpp",
	};
	std::vector<std::string_view> const forbidden_boundary_markers = {
		"core_host_context.h",
		"CoreHostThreadContext",
		"CoreHostThreadHooks",
		"InvokeOnMain(",
		"PostToMain(",
		"FlushMainJobsForHost(",
		"provider_factory_registry.h",
		"FinalizeProviderFactoryRegistries(",
		"FreezeProviderFactoryRegistries(",
	};

	std::vector<std::string> unexpected_hits;
	for (auto const& path : hot_paths) {
		ASSERT_TRUE(std::filesystem::exists(path)) << path;
		for (auto const marker : forbidden_boundary_markers) {
			auto hits = FindLiteralHits(path, marker);
			unexpected_hits.insert(unexpected_hits.end(), hits.begin(), hits.end());
		}
	}

	EXPECT_TRUE(unexpected_hits.empty()) << JoinLines(unexpected_hits);
}

TEST(host_boundary_policy, shared_mkv_subtitle_conditional_backend_sources_live_in_named_cmake_packs) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_common_sources = {
		"src/mkv_wrap_common.cpp",
	};
	std::set<std::string> const expected_legacy_sources = {
		"src/MatroskaParser.c",
		"src/mkv_wrap.cpp",
	};
	std::set<std::string> const expected_libmatroska_sources = {
		"src/mkv_wrap_libmatroska.cpp",
	};

	auto common_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_MKV_SUBTITLE_COMMON_SOURCES");
	auto legacy_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_MKV_SUBTITLE_LEGACY_BACKEND_SOURCES");
	auto libmatroska_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_MKV_SUBTITLE_LIBMATROSKA_BACKEND_SOURCES");
	auto common_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_MKV_SUBTITLE_COMMON_SOURCES}");
	auto legacy_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_MKV_SUBTITLE_LEGACY_BACKEND_SOURCES}");
	auto libmatroska_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_MKV_SUBTITLE_LIBMATROSKA_BACKEND_SOURCES}");

	EXPECT_EQ(expected_common_sources, common_sources);
	EXPECT_EQ(expected_legacy_sources, legacy_sources);
	EXPECT_EQ(expected_libmatroska_sources, libmatroska_sources);
	EXPECT_FALSE(common_expansion_hits.empty());
	EXPECT_FALSE(legacy_expansion_hits.empty());
	EXPECT_FALSE(libmatroska_expansion_hits.empty());
}

TEST(host_boundary_policy, shared_inspect_open_query_services_and_provider_diagnostics_stay_wx_free) {
	auto const root = ProjectRoot();
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "app_runtime_facilities.cpp",
		root / "src" / "app_runtime_init.cpp",
		root / "src" / "locale_pick.cpp",
		root / "src" / "media_inspect_service.cpp",
		root / "src" / "media_open_contract.h",
		root / "src" / "playback_query_service.cpp",
		root / "src" / "project_open_service.cpp",
		root / "src" / "cache_cleanup.h",
		root / "src" / "cache_cleanup.cpp",
		root / "src" / "provider_catalog.h",
		root / "src" / "provider_catalog_builder.h",
		root / "src" / "provider_catalog_builder.cpp",
		root / "src" / "provider_factory_entry.h",
		root / "src" / "provider_index_cache.h",
		root / "src" / "provider_index_cache.cpp",
		root / "src" / "provider_open_policy.h",
		root / "src" / "provider_open_policy.cpp",
		root / "src" / "provider_selection_diagnostics.h",
	};

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}

	auto const media_inspect_service_cpp = root / "src" / "media_inspect_service.cpp";
	auto media_inspect_provider_diagnostics_hits = FindLiteralHits(media_inspect_service_cpp, "provider_selection_diagnostics.h");
	EXPECT_FALSE(media_inspect_provider_diagnostics_hits.empty());
}

TEST(host_boundary_policy, src_sources_avoid_locale_encoded_path_string_extractors) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	auto hits = FindLiteralHitsRecursive(src_root, ".string()");
	auto generic_hits = FindLiteralHitsRecursive(src_root, ".generic_string()");
	hits.insert(hits.end(), generic_hits.begin(), generic_hits.end());
	EXPECT_TRUE(hits.empty()) << JoinLines(hits);
}

TEST(host_boundary_policy, src_sources_avoid_implicit_narrow_path_construction) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	std::vector<std::string> unexpected_hits;
	for (auto const& needle : {"agi::fs::path(", "std::filesystem::path(", "stdfs::path("}) {
		for (auto const& hit : FindLiteralHitsRecursive(src_root, needle)) {
			if (std::string_view(needle) == "agi::fs::path(" && hit.find("agi::fs::path()") != std::string::npos)
				continue;
			if (std::string_view(needle) == "stdfs::path(" && hit.find("stdfs::path(path).parent_path()") != std::string::npos)
				continue;
			unexpected_hits.push_back(hit);
		}
	}

	EXPECT_TRUE(unexpected_hits.empty()) << JoinLines(unexpected_hits);
}

TEST(host_boundary_policy, common_sources_avoid_locale_encoded_path_string_extractors) {
	auto const root = ProjectRoot();
	auto const common_root = root / "libaegisub" / "common";
	ASSERT_TRUE(std::filesystem::exists(common_root));

	std::vector<std::string> unexpected_hits;
	for (auto const& needle : {".string()", ".generic_string()"}) {
		for (auto const& hit : FindLiteralHitsRecursive(common_root, needle)) {
			if (hit.find("libaegisub/common/fs.cpp") != std::string::npos
				&& hit.find("return value.string();") != std::string::npos)
				continue;
			if (hit.find("libaegisub/common/fs.cpp") != std::string::npos
				&& hit.find("return value.generic_string();") != std::string::npos)
				continue;
			unexpected_hits.push_back(hit);
		}
	}

	EXPECT_TRUE(unexpected_hits.empty()) << JoinLines(unexpected_hits);
}

TEST(host_boundary_policy, common_sources_avoid_implicit_narrow_path_construction) {
	auto const root = ProjectRoot();
	auto const common_root = root / "libaegisub" / "common";
	ASSERT_TRUE(std::filesystem::exists(common_root));

	std::vector<std::string> unexpected_hits;
	for (auto const& needle : {"agi::fs::path(", "std::filesystem::path(", "bfs::path("}) {
		for (auto const& hit : FindLiteralHitsRecursive(common_root, needle)) {
			if (std::string_view(needle) == "agi::fs::path(" && hit.find("agi::fs::path()") != std::string::npos)
				continue;
			unexpected_hits.push_back(hit);
		}
	}

	EXPECT_TRUE(unexpected_hits.empty()) << JoinLines(unexpected_hits);
}

TEST(host_boundary_policy, shared_runtime_common_init_reachable_set_stays_split_from_gui_shell) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const app_runtime_h = root / "src" / "app_runtime.h";
	auto const app_runtime_cpp = root / "src" / "app_runtime.cpp";
	auto const gui_wx_runtime_entry_host_h = root / "src" / "gui_wx_runtime_entry_host.h";
	auto const gui_wx_runtime_entry_host_cpp = root / "src" / "gui_wx_runtime_entry_host.cpp";
	auto const runtime_bootstrap_ui_host_h = root / "src" / "runtime_bootstrap_ui_host.h";
	auto const runtime_process_host_h = root / "src" / "runtime_process_host.h";
	auto const runtime_optional_facility_host_h = root / "src" / "runtime_optional_facility_host.h";
	auto const app_runtime_facilities_cpp = root / "src" / "app_runtime_facilities.cpp";
	auto const app_runtime_init_cpp = root / "src" / "app_runtime_init.cpp";
	auto const aegisublocale_cpp = root / "src" / "aegisublocale.cpp";
	auto const headless_process_entry_cpp = root / "src" / "headless_process_entry.cpp";
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";

	auto main_gui_runtime_entry_host_hits = FindLiteralHits(main_cpp, "gui_wx_runtime_entry_host.h");
	auto main_gui_dispatch_event_hits = FindLiteralHits(main_cpp, "gui_wx_dispatch_event.h");
	auto main_gui_runtime_host_hits = FindLiteralHits(main_cpp, "gui_wx_runtime_host.h");
	auto main_gui_locale_host_hits = FindLiteralHits(main_cpp, "gui_wx_locale_host.h");
	auto main_gui_bootstrap_ui_host_hits = FindLiteralHits(main_cpp, "gui_wx_bootstrap_ui_host.h");
	auto main_queue_event_hits = FindLiteralHits(main_cpp, "QueueEvent(");
	auto main_is_main_thread_hits = FindLiteralHits(main_cpp, "wxIsMainThread()");
	auto main_runtime_options_local_hits = FindLiteralHits(main_cpp, "AppRuntimeInitOptions runtime_options");
	auto main_initialize_commands_hits = FindLiteralHits(main_cpp, "initialize_commands = true");
	auto main_install_png_handler_hits = FindLiteralHits(main_cpp, "install_png_handler = true");

	auto app_runtime_header_wx_hits = FindWxMarkers(app_runtime_h);
	auto gui_runtime_entry_host_h_wx_hits = FindWxMarkers(gui_wx_runtime_entry_host_h);
	auto runtime_bootstrap_ui_header_wx_hits = FindWxMarkers(runtime_bootstrap_ui_host_h);
	auto runtime_process_host_header_wx_hits = FindWxMarkers(runtime_process_host_h);
	auto runtime_optional_facility_host_header_wx_hits = FindWxMarkers(runtime_optional_facility_host_h);
	auto app_runtime_gui_runtime_host_hits = FindLiteralHits(app_runtime_cpp, "gui_wx_runtime_host.h");
	auto app_runtime_gui_locale_host_hits = FindLiteralHits(app_runtime_cpp, "gui_wx_locale_host.h");
	auto app_runtime_gui_bootstrap_ui_host_hits = FindLiteralHits(app_runtime_cpp, "gui_wx_bootstrap_ui_host.h");
	auto app_runtime_bootstrap_ui_hits = FindLiteralHits(app_runtime_cpp, "wx_app_bootstrap_ui_services.h");
	auto app_runtime_main_header_hits = FindLiteralHits(app_runtime_cpp, "main.h");
	auto app_runtime_facilities_wx_hits = FindWxMarkers(app_runtime_facilities_cpp);
	auto app_runtime_facilities_gui_runtime_host_hits = FindLiteralHits(app_runtime_facilities_cpp, "gui_wx_runtime_host.h");
	auto app_runtime_facilities_gui_locale_host_hits = FindLiteralHits(app_runtime_facilities_cpp, "gui_wx_locale_host.h");
	auto app_runtime_facilities_gui_bootstrap_ui_host_hits = FindLiteralHits(app_runtime_facilities_cpp, "gui_wx_bootstrap_ui_host.h");
	auto app_runtime_facilities_bootstrap_ui_hits = FindLiteralHits(app_runtime_facilities_cpp, "wx_app_bootstrap_ui_services.h");
	auto app_runtime_facilities_main_header_hits = FindLiteralHits(app_runtime_facilities_cpp, "main.h");
	auto app_runtime_init_wx_hits = FindWxMarkers(app_runtime_init_cpp);
	auto app_runtime_init_gui_runtime_host_hits = FindLiteralHits(app_runtime_init_cpp, "gui_wx_runtime_host.h");
	auto app_runtime_init_gui_locale_host_hits = FindLiteralHits(app_runtime_init_cpp, "gui_wx_locale_host.h");
	auto app_runtime_init_gui_bootstrap_ui_host_hits = FindLiteralHits(app_runtime_init_cpp, "gui_wx_bootstrap_ui_host.h");
	auto app_runtime_init_bootstrap_ui_hits = FindLiteralHits(app_runtime_init_cpp, "wx_app_bootstrap_ui_services.h");
	auto app_runtime_init_main_header_hits = FindLiteralHits(app_runtime_init_cpp, "main.h");
	auto app_runtime_locale_wx_hits = FindWxMarkers(aegisublocale_cpp);
	auto gui_runtime_entry_runtime_host_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "gui_wx_runtime_host.h");
	auto gui_runtime_entry_locale_host_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "gui_wx_locale_host.h");
	auto gui_runtime_entry_bootstrap_ui_host_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "gui_wx_bootstrap_ui_host.h");
	auto gui_runtime_entry_dispatch_event_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "gui_wx_dispatch_event.h");
	auto gui_runtime_entry_queue_event_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "QueueEvent(");
	auto gui_runtime_entry_is_main_thread_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "wxIsMainThread()");
	auto gui_runtime_entry_init_options_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "AppRuntimeInitOptions options");
	auto gui_runtime_entry_initialize_commands_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "initialize_commands = true");
	auto gui_runtime_entry_install_png_handler_hits = FindLiteralHits(gui_wx_runtime_entry_host_cpp, "install_png_handler = true");

	auto process_entry_wx_hits = FindWxMarkers(headless_process_entry_cpp);
	auto process_entry_main_header_hits = FindLiteralHits(headless_process_entry_cpp, "main.h");
	auto process_entry_gui_runtime_host_hits = FindLiteralHits(headless_process_entry_cpp, "gui_wx_runtime_host.h");

	auto bootstrap_wx_hits = FindWxMarkers(headless_runtime_bootstrap_cpp);
	auto bootstrap_main_header_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "main.h");
	auto bootstrap_gui_runtime_host_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "gui_wx_runtime_host.h");
	auto bootstrap_gui_bootstrap_ui_host_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "gui_wx_bootstrap_ui_host.h");
	auto bootstrap_bootstrap_ui_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wx_app_bootstrap_ui_services.h");

	EXPECT_FALSE(main_gui_runtime_entry_host_hits.empty());
	EXPECT_TRUE(main_gui_dispatch_event_hits.empty()) << JoinLines(main_gui_dispatch_event_hits);
	EXPECT_TRUE(main_gui_runtime_host_hits.empty()) << JoinLines(main_gui_runtime_host_hits);
	EXPECT_TRUE(main_gui_locale_host_hits.empty()) << JoinLines(main_gui_locale_host_hits);
	EXPECT_TRUE(main_gui_bootstrap_ui_host_hits.empty()) << JoinLines(main_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(main_queue_event_hits.empty()) << JoinLines(main_queue_event_hits);
	EXPECT_TRUE(main_is_main_thread_hits.empty()) << JoinLines(main_is_main_thread_hits);
	EXPECT_TRUE(main_runtime_options_local_hits.empty()) << JoinLines(main_runtime_options_local_hits);
	EXPECT_TRUE(main_initialize_commands_hits.empty()) << JoinLines(main_initialize_commands_hits);
	EXPECT_TRUE(main_install_png_handler_hits.empty()) << JoinLines(main_install_png_handler_hits);

	EXPECT_TRUE(app_runtime_header_wx_hits.empty()) << JoinLines(app_runtime_header_wx_hits);
	EXPECT_TRUE(gui_runtime_entry_host_h_wx_hits.empty()) << JoinLines(gui_runtime_entry_host_h_wx_hits);
	EXPECT_TRUE(runtime_bootstrap_ui_header_wx_hits.empty()) << JoinLines(runtime_bootstrap_ui_header_wx_hits);
	EXPECT_TRUE(runtime_process_host_header_wx_hits.empty()) << JoinLines(runtime_process_host_header_wx_hits);
	EXPECT_TRUE(runtime_optional_facility_host_header_wx_hits.empty()) << JoinLines(runtime_optional_facility_host_header_wx_hits);
	EXPECT_TRUE(app_runtime_gui_runtime_host_hits.empty()) << JoinLines(app_runtime_gui_runtime_host_hits);
	EXPECT_TRUE(app_runtime_gui_locale_host_hits.empty()) << JoinLines(app_runtime_gui_locale_host_hits);
	EXPECT_TRUE(app_runtime_gui_bootstrap_ui_host_hits.empty()) << JoinLines(app_runtime_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(app_runtime_bootstrap_ui_hits.empty()) << JoinLines(app_runtime_bootstrap_ui_hits);
	EXPECT_TRUE(app_runtime_main_header_hits.empty()) << JoinLines(app_runtime_main_header_hits);
	EXPECT_TRUE(app_runtime_facilities_wx_hits.empty()) << JoinLines(app_runtime_facilities_wx_hits);
	EXPECT_TRUE(app_runtime_facilities_gui_runtime_host_hits.empty()) << JoinLines(app_runtime_facilities_gui_runtime_host_hits);
	EXPECT_TRUE(app_runtime_facilities_gui_locale_host_hits.empty()) << JoinLines(app_runtime_facilities_gui_locale_host_hits);
	EXPECT_TRUE(app_runtime_facilities_gui_bootstrap_ui_host_hits.empty()) << JoinLines(app_runtime_facilities_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(app_runtime_facilities_bootstrap_ui_hits.empty()) << JoinLines(app_runtime_facilities_bootstrap_ui_hits);
	EXPECT_TRUE(app_runtime_facilities_main_header_hits.empty()) << JoinLines(app_runtime_facilities_main_header_hits);
	EXPECT_TRUE(app_runtime_init_wx_hits.empty()) << JoinLines(app_runtime_init_wx_hits);
	EXPECT_TRUE(app_runtime_init_gui_runtime_host_hits.empty()) << JoinLines(app_runtime_init_gui_runtime_host_hits);
	EXPECT_TRUE(app_runtime_init_gui_locale_host_hits.empty()) << JoinLines(app_runtime_init_gui_locale_host_hits);
	EXPECT_TRUE(app_runtime_init_gui_bootstrap_ui_host_hits.empty()) << JoinLines(app_runtime_init_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(app_runtime_init_bootstrap_ui_hits.empty()) << JoinLines(app_runtime_init_bootstrap_ui_hits);
	EXPECT_TRUE(app_runtime_init_main_header_hits.empty()) << JoinLines(app_runtime_init_main_header_hits);
	EXPECT_TRUE(app_runtime_locale_wx_hits.empty()) << JoinLines(app_runtime_locale_wx_hits);
	EXPECT_FALSE(gui_runtime_entry_runtime_host_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_locale_host_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_bootstrap_ui_host_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_dispatch_event_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_queue_event_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_is_main_thread_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_init_options_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_initialize_commands_hits.empty());
	EXPECT_FALSE(gui_runtime_entry_install_png_handler_hits.empty());

	EXPECT_TRUE(process_entry_wx_hits.empty()) << JoinLines(process_entry_wx_hits);
	EXPECT_TRUE(process_entry_main_header_hits.empty()) << JoinLines(process_entry_main_header_hits);
	EXPECT_TRUE(process_entry_gui_runtime_host_hits.empty()) << JoinLines(process_entry_gui_runtime_host_hits);

	EXPECT_TRUE(bootstrap_wx_hits.empty()) << JoinLines(bootstrap_wx_hits);
	EXPECT_TRUE(bootstrap_main_header_hits.empty()) << JoinLines(bootstrap_main_header_hits);
	EXPECT_TRUE(bootstrap_gui_runtime_host_hits.empty()) << JoinLines(bootstrap_gui_runtime_host_hits);
	EXPECT_TRUE(bootstrap_gui_bootstrap_ui_host_hits.empty()) << JoinLines(bootstrap_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(bootstrap_bootstrap_ui_hits.empty()) << JoinLines(bootstrap_bootstrap_ui_hits);
}

TEST(host_boundary_policy, gui_runtime_and_bootstrap_host_headers_stay_localized_to_gui_entry) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";

	std::set<std::string> const expected_runtime_entry_host_includers = {
		"src/gui_wx_runtime_entry_host.cpp",
		"src/main.cpp",
	};
	std::set<std::string> const expected_dispatch_event_includers = {
		"src/gui_wx_dispatch_event.cpp",
		"src/gui_wx_runtime_entry_host.cpp",
	};
	std::set<std::string> const expected_runtime_host_includers = {
		"src/gui_wx_runtime_entry_host.cpp",
		"src/gui_wx_runtime_host.cpp",
	};
	std::set<std::string> const expected_runtime_process_host_includers = {
		"src/app_runtime.h",
		"src/gui_wx_runtime_host.h",
	};
	std::set<std::string> const expected_runtime_optional_facility_host_includers = {
		"src/app_runtime.h",
		"src/gui_wx_runtime_host.h",
	};
	std::set<std::string> const expected_locale_host_includers = {
		"src/gui_wx_runtime_entry_host.cpp",
		"src/gui_wx_locale_host.cpp",
	};
	std::set<std::string> const expected_bootstrap_host_includers = {
		"src/gui_wx_runtime_entry_host.cpp",
		"src/gui_wx_bootstrap_ui_host.cpp",
	};
	std::set<std::string> const expected_bootstrap_ui_includers = {
		"src/gui_wx_bootstrap_ui_host.cpp",
	};

	auto runtime_entry_host_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_runtime_entry_host.h");
	auto dispatch_event_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_dispatch_event.h");
	auto runtime_host_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_runtime_host.h");
	auto runtime_process_host_includers = FindFilesContainingLiteralInTree(root, src_root, "runtime_process_host.h");
	auto runtime_optional_facility_host_includers = FindFilesContainingLiteralInTree(root, src_root, "runtime_optional_facility_host.h");
	auto locale_host_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_locale_host.h");
	auto bootstrap_host_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_bootstrap_ui_host.h");
	auto bootstrap_ui_includers = FindFilesContainingLiteralInTree(root, src_root, "wx_app_bootstrap_ui_services.h");

	EXPECT_EQ(expected_runtime_entry_host_includers, runtime_entry_host_includers);
	EXPECT_EQ(expected_dispatch_event_includers, dispatch_event_includers);
	EXPECT_EQ(expected_runtime_host_includers, runtime_host_includers);
	EXPECT_EQ(expected_runtime_process_host_includers, runtime_process_host_includers);
	EXPECT_EQ(expected_runtime_optional_facility_host_includers, runtime_optional_facility_host_includers);
	EXPECT_EQ(expected_locale_host_includers, locale_host_includers);
	EXPECT_EQ(expected_bootstrap_host_includers, bootstrap_host_includers);
	EXPECT_EQ(expected_bootstrap_ui_includers, bootstrap_ui_includers);
}

TEST(host_boundary_policy, explicit_wx_surface_inventory_stays_current) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	std::set<std::string> const expected_explicit_wx_surfaces = {
		"src/gui_wx_bootstrap_ui_host.cpp",
		"src/gui_wx_bootstrap_ui_host.h",
		"src/gui_wx_dispatch_event.cpp",
		"src/gui_wx_dispatch_event.h",
		"src/wx_app_bootstrap_ui_services.h",
		"src/wx_automation_dialog.cpp",
		"src/wx_automation_dialog.h",
		"src/wx_automation_runtime.cpp",
		"src/wx_automation_runtime.h",
		"src/wx_file_dialog_services.h",
		"src/wx_frame_main_dialog_ui_host.h",
		"src/wx_frame_main_request_host.h",
		"src/wx_frame_main_runtime_host.h",
		"src/wx_preferences_ui_host.h",
		"src/wx_style_editor_ui_host.h",
		"src/gui_wx_locale_host.cpp",
		"src/gui_wx_locale_host.h",
		"src/gui_wx_runtime_entry_host.cpp",
		"src/gui_wx_runtime_entry_host.h",
		"src/gui_wx_runtime_host.cpp",
		"src/gui_wx_runtime_host.h",
		"src/gui_wx_subtitle_format_host.cpp",
		"src/gui_wx_subtitle_format_host.h",
		"src/video_frame_wx.h",
		"src/watched_file_wx.cpp",
		"src/wx_message_box_ui_services.h",
		"src/wx_audio_controller_power_host.cpp",
		"src/wx_single_choice_dialog.h",
		"src/wx_translation_service.cpp",
		"src/wx_translation_service.h",
	};

	std::set<std::string> actual_explicit_wx_surfaces;
	for (auto const& entry : std::filesystem::directory_iterator(src_root)) {
		if (!entry.is_regular_file())
			continue;

		auto relative = agi::fs::PathToGenericString(std::filesystem::relative(entry.path(), root));
		auto const filename = agi::fs::PathToString(entry.path().filename());
		if (filename.find("wx") == std::string::npos)
			continue;
		actual_explicit_wx_surfaces.insert(relative);
	}

	EXPECT_EQ(expected_explicit_wx_surfaces, actual_explicit_wx_surfaces);
}
