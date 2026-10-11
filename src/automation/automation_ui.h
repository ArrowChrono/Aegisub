#pragma once

#include <libaegisub/background_runner.h>
#include <libaegisub/color.h>
#include <libaegisub/fs_fwd.h>

#include <functional>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace Automation4 {

struct AutomationDialogLabel {
	std::string label;
};

struct AutomationDialogEdit {
	std::string value;
	bool multiline = false;
};

struct AutomationDialogColor {
	agi::Color value;
	bool alpha = false;
};

struct AutomationDialogIntEdit {
	int value = 0;
	int min = std::numeric_limits<int>::lowest();
	int max = std::numeric_limits<int>::max();
};

struct AutomationDialogFloatEdit {
	double value = 0.0;
	double min = std::numeric_limits<double>::lowest();
	double max = std::numeric_limits<double>::max();
	double step = 0.0;
};

struct AutomationDialogDropdown {
	std::vector<std::string> items;
	std::string value;
};

struct AutomationDialogCheckbox {
	std::string label;
	bool value = false;
};

using AutomationDialogValue = std::variant<AutomationDialogLabel, AutomationDialogEdit,
										   AutomationDialogColor, AutomationDialogIntEdit, AutomationDialogFloatEdit,
										   AutomationDialogDropdown, AutomationDialogCheckbox>;

struct AutomationDialogControl {
	std::string name;
	std::string hint;
	int x = 0;
	int y = 0;
	int width = 1;
	int height = 1;
	AutomationDialogValue value;
};

enum class AutomationDialogButtonRole {
	Custom,
	Ok,
	Yes,
	Save,
	Apply,
	Close,
	No,
	Cancel,
	Help,
	ContextHelp,
};

struct AutomationDialogButton {
	AutomationDialogButtonRole role = AutomationDialogButtonRole::Custom;
	std::string label;
};

class AutomationDialog {
	public:
	std::vector<AutomationDialogControl> controls;
	std::vector<AutomationDialogButton> buttons;
	bool use_buttons = false;
	int button_pushed = -1;
	double model_build_duration_ms = 0.0;

	virtual ~AutomationDialog() = default;
	virtual std::string Serialise() = 0;
	virtual void Unserialise(std::string const& serialised) = 0;
};

struct AutomationOpenFileDialogRequest {
	std::string message;
	std::string dir;
	std::string file;
	std::string wildcard;
	bool multiple = false;
	bool must_exist = true;
};

struct AutomationSaveFileDialogRequest {
	std::string message;
	std::string dir;
	std::string file;
	std::string wildcard;
	bool prompt_overwrite = true;
};

class AutomationProgress : public agi::ProgressSink {
	public:
	virtual void ShowDialog(AutomationDialog& dialog) = 0;
	virtual std::vector<agi::fs::path> RequestOpenFiles(AutomationOpenFileDialogRequest const& request) = 0;
	virtual agi::fs::path RequestSaveFile(AutomationSaveFileDialogRequest const& request) = 0;
	virtual int GetTraceLevel() const = 0;
};

class AutomationRunner {
	public:
	virtual ~AutomationRunner() = default;
	virtual void Run(std::function<void(AutomationProgress *)> task) = 0;
};

}
