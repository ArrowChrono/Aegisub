#pragma once

#include <string>
#include <vector>

namespace aegisub::fontcollector_subcommand {

enum class Operation {
	Check,
	Collect,
	Validate,
	List,
	Normalize,
	Inventory,
};

struct Options {
	Operation operation = Operation::Check;
	std::vector<std::string> inputs;
	std::string encoding;
	std::string matcher = "platform";
	bool details = false;
	bool json = false;
	bool recursive = false;
	bool strict = false;
	bool quiet = false;
	std::vector<std::string> additional_fonts;
	std::vector<std::string> additional_fonts_recursive;
	bool exclude_system_fonts = false;
	std::string destination;
	bool copy_to_script_directory = false;
	std::string normalization_target = "localized";
	int low_frequency_threshold = 5;
};

int Run(Options const& options);

}
