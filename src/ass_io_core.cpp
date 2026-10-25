// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
// Copyright (c) 2026, MIR
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
//
// Aegisub Project http://www.aegisub.org/

#include "ass_io_core.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_entry.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_parser.h"
#include "ass_style.h"
#include "ass_time_projection.h"
#include "string_codec.h"
#include "text_file_reader.h"
#include "text_file_writer.h"

#include <libaegisub/ass/uuencode.h>
#include <libaegisub/fs.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/vfr.h>

#ifdef _WIN32
#define LINEBREAK "\r\n"
#else
#define LINEBREAK "\n"
#endif

namespace {
const char *format(AssEntryGroup group) {
	if (group == AssEntryGroup::DIALOGUE)
		return "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text" LINEBREAK;
	if (group == AssEntryGroup::STYLE)
		return "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding" LINEBREAK;
	return nullptr;
}

struct Writer {
	TextFileWriter file;
	AssEntryGroup group = AssEntryGroup::INFO;
	agi::vfr::Framerate const* fps = nullptr;
	AssWriteOptions const& options;

	Writer(agi::fs::path const& filename, std::string const& encoding, AssWriteOptions const& options)
	: file(filename, encoding)
	, options(options)
	{
		file.WriteLineToFile("[Script Info]");
		if (options.write_generator)
			file.WriteLineToFile(options.generator_header, false);
	}

	template<typename T>
	void Write(T const& list) {
		for (auto const& line : list) {
			if (line.Group() != group) {
				// Add a blank line between each group
				file.WriteLineToFile("");

				file.WriteLineToFile(line.GroupHeader());
				if (const char *str = format(line.Group()))
					file.WriteLineToFile(str, false);

				group = line.Group();
			}

			file.WriteLineToFile(line.GetEntryData());
		}
	}

	void Write(std::vector<AssInfo> const& list) {
		for (auto const& line : list) {
			if (agi::util::strings::iequals(line.Key(), "scripttype"))
				file.WriteLineToFile("ScriptType: v4.00+");
			else
				file.WriteLineToFile(line.GetEntryData());
		}
	}

	void Write(EntryList<AssDialogue> const& list) {
		for (auto const& line : list) {
			if (line.Group() != group) {
				file.WriteLineToFile("");
				file.WriteLineToFile(line.GroupHeader());
				if (const char *str = format(line.Group()))
					file.WriteLineToFile(str, false);
				group = line.Group();
			}

			file.WriteLineToFile(SerializeAssDialogueForOutput(line, AssTimeOutputMode::LegacyRounding, fps));
		}
	}

	void Write(ProjectProperties const& properties) {
		file.WriteLineToFile("");
		file.WriteLineToFile("[Aegisub Project Garbage]");

		WriteIfNotEmpty("Automation Scripts: ", properties.automation_scripts);
		WriteIfNotEmpty("Export Filters: ", properties.export_filters);
		WriteIfNotEmpty("Export Encoding: ", properties.export_encoding);
		for (auto const& [name, settings] : properties.automation_settings) {
			std::string line = "Automation Settings ";
			line.append(name).append(": ").append(settings);
			file.WriteLineToFile(line);
		}
		WriteIfNotEmpty("Last Style Storage: ", properties.style_storage);
		WriteIfNotEmpty("Audio File: ", properties.audio_file);
		WriteIfNotEmpty("Video File: ", properties.video_file);
		WriteIfNotEmpty("Timecodes File: ", properties.timecodes_file);
		WriteIfNotEmpty("Keyframes File: ", properties.keyframes_file);
		WriteIfNotEmpty("Secondary Subtitles File: ", properties.secondary_subtitles_file);

		WriteIfNotZero("Video AR Mode: ", properties.ar_mode);
		WriteIfNotZero("Video AR Value: ", properties.ar_value);

		if (options.write_ui_state) {
			WriteIfNotZero("Video Zoom Percent: ", properties.video_zoom);
			WriteIfNotZero("Scroll Position: ", properties.scroll_position);
			WriteIfNotZero("Active Line: ", properties.active_row);
			WriteIfNotZero("Video Position: ", properties.video_position);
		}
	}

	void WriteIfNotEmpty(const char *key, std::string const& value) {
		if (!value.empty())
			file.WriteLineToFile(key + value);
	}

	template<typename Number>
	void WriteIfNotZero(const char *key, Number n) {
		if (n != Number{})
			file.WriteLineToFile(key + std::to_string(n));
	}

	void WriteExtradata(std::vector<ExtradataEntry> const& extradata) {
		if (extradata.empty())
			return;

		group = AssEntryGroup::EXTRADATA;
		file.WriteLineToFile("");
		file.WriteLineToFile("[Aegisub Extradata]");
		for (auto const& edi : extradata) {
			std::string line = "Data: ";
			line.reserve(16 + edi.key.size() + edi.value.size());
			line.append(std::to_string(edi.id));
			line.push_back(',');
			line.append(inline_string_encode(edi.key));
			line.push_back(',');
			std::string encoded_data = inline_string_encode(edi.value);
			if (4*edi.value.size() < 3*encoded_data.size()) {
				// the inline_string encoding grew the data by more than uuencoding would
				// so base64 encode it instead
				line.push_back('u'); // marker for uuencoding
				line.append(agi::ass::UUEncode(edi.value.c_str(), edi.value.c_str() + edi.value.size(), false));
			}
			else {
				line.push_back('e'); // marker for inline_string encoding (escaping)
				line.append(encoded_data);
			}
			file.WriteLineToFile(line);
		}
	}

	void Close() {
		file.Close();
	}
};
}

void ReadAssFileForCore(AssFile *target, agi::fs::path const& filename, std::string const& encoding) {
	int version = !agi::fs::HasExtension(filename, "ssa");

	TextFileReader file(filename, encoding);
	AssParser parser(target, version);
	while (file.HasMoreLines())
		parser.AddLine(file.ReadLineFromFile());
}

AssFile ReadAssFileForCore(agi::fs::path const& filename, std::string const& encoding) {
	AssFile file;
	ReadAssFileForCore(&file, filename, encoding);
	return file;
}

char const* AssStorageLineBreak() {
	return LINEBREAK;
}

void WriteAssFileForCore(const AssFile *src, agi::fs::path const& filename, agi::vfr::Framerate const& fps, std::string const& encoding, AssWriteOptions const& options) {
	Writer writer(filename, encoding, options);
	writer.fps = fps.IsLoaded() ? &fps : nullptr;
	writer.Write(src->Info);
	if (options.write_project_garbage)
		writer.Write(src->Properties);
	writer.Write(src->Styles);
	writer.Write(src->Attachments);
	writer.Write(src->Events);
	if (options.write_extradata)
		writer.WriteExtradata(src->Extradata);
	writer.Close();
}
