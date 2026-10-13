import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "src/command_help.generated.h"
STRING = r'"(?:[^"\\]|\\.)*"'


def generate():
    entries = {}
    for path in sorted((ROOT / "src/command").glob("*.cpp")):
        source = path.read_text(encoding="utf-8")
        names = list(re.finditer(rf"CMD_NAME\(\s*({STRING})\s*\)", source))
        for index, name in enumerate(names):
            end = names[index + 1].start() if index + 1 < len(names) else len(source)
            help_text = re.search(rf"STR_HELP\(\s*({STRING})\s*\)", source[name.end():end])
            if not help_text:
                raise ValueError(f"{path.relative_to(ROOT)}: missing literal STR_HELP for {name[1]}")
            entries[name[1]] = help_text[1]
        for group in re.finditer(
            rf"COMMAND_GROUP\(\s*\w+\s*,\s*({STRING})\s*,\s*{STRING}\s*,\s*{STRING}\s*,\s*({STRING})\s*\)",
            source,
        ):
            entries[group[1]] = group[2]
    rows = "\n".join(f"\t\t{{{name}, {help_text}}}," for name, help_text in sorted(entries.items()))
    return """#pragma once

#include <algorithm>
#include <iterator>
#include <string_view>

namespace agi {
inline std::string_view CommandHelp(std::string_view name) {
    struct Entry {
        std::string_view name;
        std::string_view help;
    };
    static constexpr Entry entries[] = {
""" + rows + """
    };
    auto found = std::lower_bound(std::begin(entries), std::end(entries), name,
        [](Entry const& entry, std::string_view key) { return entry.name < key; });
    return found != std::end(entries) && found->name == name ? found->help : std::string_view{};
}
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    generated = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != generated:
            raise SystemExit("Command help catalog is stale; run tools/generate-command-help.py")
    else:
        OUTPUT.write_text(generated, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
