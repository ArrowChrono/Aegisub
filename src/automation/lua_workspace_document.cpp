#include "lua_workspace_document.h"

#include "karaoke_line_classifier.h"
#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../include/aegisub/context.h"
#include "../selection_controller.h"
#include "../subs_controller.h"

#include <libaegisub/charset_conv.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <iterator>
#include <stdexcept>
#include <utility>

namespace Automation4 {
namespace {
LuaWorkspaceDocumentResult Error(std::string message) {
	return {.state = LuaWorkspaceDocumentState::Error, .message = std::move(message)};
}

LuaWorkspaceSnapshot Snapshot(AssDialogue const& line) {
	return {.text = line.Text.get(), .comment = line.Comment, .effect = line.Effect.get(), .style = line.Style.get()};
}

std::string ReadBytes(agi::fs::path const& filename) {
	auto input = agi::io::Open(filename, true);
	std::string result(std::istreambuf_iterator<char>(*input), {});
	if (input->bad())
		throw agi::io::IOError("Unable to read the complete Lua source file");
	return result;
}

std::optional<std::string> ValidateText(std::string_view source) {
	if (source.find('\0') != std::string_view::npos || source.starts_with("\x1b"))
		return "Lua Workspace accepts text source, not binary data";
	try {
		agi::charset::IconvWrapper converter("UTF-8", "UTF-8", false);
		converter.Convert(source.data(), source.size());
	}
	catch (agi::Exception const&) {
		return "Lua source must be valid UTF-8 text";
	}
	return std::nullopt;
}

std::string Difference(LuaWorkspaceSnapshot const& before, LuaWorkspaceSnapshot const& after) {
	std::string fields;
	if (before.text != after.text)
		fields += "Text ";
	if (before.comment != after.comment)
		fields += "Comment ";
	if (before.effect != after.effect)
		fields += "Effect ";
	if (before.style != after.style)
		fields += "Style ";
	return "Target changed outside Lua Workspace: " + fields;
}
}

LuaWorkspaceDocument::LuaWorkspaceDocument(LuaWorkspaceDocumentKind value) : kind(value) {}

std::unique_ptr<LuaWorkspaceDocument> LuaWorkspaceDocument::OpenCode(agi::Context *context, int dialogue_id, LuaWorkspaceDocumentResult& result) {
	if (!context || !context->subsController || !context->selectionController) {
		result = Error("The subtitle document is unavailable");
		return nullptr;
	}
	auto document = std::unique_ptr<LuaWorkspaceDocument>(new LuaWorkspaceDocument(LuaWorkspaceDocumentKind::KaraokeCode));
	document->context = context;
	document->document_generation = context->subsController->GetDocumentGeneration();
	document->dialogue_id = dialogue_id;
	document->display_name = "Code line ID " + std::to_string(dialogue_id);
	document->source_identity = "aegisub://lua/" + std::to_string(document->document_generation) + "/" + std::to_string(dialogue_id);
	result = document->Reload();
	return result.Succeeded() ? std::move(document) : nullptr;
}

std::unique_ptr<LuaWorkspaceDocument> LuaWorkspaceDocument::OpenFile(agi::fs::path const& filename, LuaWorkspaceDocumentResult& result) {
	auto document = std::unique_ptr<LuaWorkspaceDocument>(new LuaWorkspaceDocument(LuaWorkspaceDocumentKind::LuaFile));
	document->filename = filename;
	document->display_name = agi::fs::PathToString(filename.filename());
	document->source_identity = agi::fs::PathToGenericString(filename);
	result = document->Reload();
	return result.Succeeded() ? std::move(document) : nullptr;
}

LuaWorkspaceDocumentResult LuaWorkspaceDocument::ReadCurrent() const {
	if (kind == LuaWorkspaceDocumentKind::KaraokeCode) {
		if (!context || !context->subsController || context->subsController->GetDocumentGeneration() != document_generation)
			return {.state = LuaWorkspaceDocumentState::Invalidated, .message = "The bound subtitle document has been replaced. Copy the source or open a new target."};
		auto line = context->selectionController->GetDialogueById(dialogue_id);
		if (!line)
			return {.state = LuaWorkspaceDocumentState::Invalidated, .message = "The bound subtitle line has been deleted. Copy the source or open a new target."};
		if (!IsKaraokeCodeLine(line->Comment, line->Effect.get()))
			return {.state = LuaWorkspaceDocumentState::Invalidated, .message = "The bound line is no longer a supported karaoke code line."};
		return {.observed = Snapshot(*line)};
	}
	try {
		return {.observed = LuaWorkspaceSnapshot{.text = ReadBytes(filename)}};
	}
	catch (agi::Exception const& error) {
		return Error(error.GetMessage());
	}
	catch (std::exception const& error) {
		return Error(error.what());
	}
}

LuaWorkspaceDocumentResult LuaWorkspaceDocument::LoadSnapshot(LuaWorkspaceSnapshot snapshot) {
	std::string loaded = snapshot.text;
	bool loaded_bom = loaded.starts_with("\xef\xbb\xbf");
	if (loaded_bom)
		loaded.erase(0, 3);
	if (auto error = ValidateText(loaded))
		return Error(*error);
	LuaWorkspaceDocumentResult result;
	if (kind == LuaWorkspaceDocumentKind::KaraokeCode) {
		auto formatted = FormatLuaSource(loaded);
		loaded = std::move(formatted.source);
		result.diagnostic = std::move(formatted.diagnostic);
	}
	baseline = std::move(snapshot);
	bom = loaded_bom;
	SetSource(std::move(loaded));
	clean_source = source;
	return result;
}

void LuaWorkspaceDocument::SetSource(std::string value) {
	if (source != value) {
		source = std::move(value);
		++revision;
	}
}

void LuaWorkspaceDocument::DiscardChanges() {
	SetSource(clean_source);
}

LuaWorkspaceDocumentResult LuaWorkspaceDocument::Check() const {
	auto current = ReadCurrent();
	if (current.Succeeded() && *current.observed != baseline) {
		current.state = LuaWorkspaceDocumentState::Conflict;
		current.message = Difference(baseline, *current.observed);
	}
	return current;
}

LuaWorkspaceDocumentResult LuaWorkspaceDocument::Reload() {
	auto current = ReadCurrent();
	if (!current.Succeeded())
		return current;
	return LoadSnapshot(std::move(*current.observed));
}

LuaWorkspaceDocumentResult LuaWorkspaceDocument::Save(LuaWorkspaceSnapshot const *confirmed_target) {
	if (auto error = ValidateText(source))
		return Error(*error);
	std::string persisted = source;
	if (kind == LuaWorkspaceDocumentKind::KaraokeCode) {
		auto serialized = SerializeLuaSource(source);
		if (!serialized.Succeeded())
			return {.state = LuaWorkspaceDocumentState::Error, .message = "Lua source could not be applied", .diagnostic = std::move(serialized.diagnostic)};
		persisted = std::move(serialized.source);
		if (persisted.find_first_of("\r\n") != std::string::npos)
			return Error("The Lua serializer did not produce a physical single line");
	}
	else if (bom)
		persisted.insert(0, "\xef\xbb\xbf");
	auto current = ReadCurrent();
	if (!current.Succeeded())
		return current;
	auto const& expected = confirmed_target ? *confirmed_target : baseline;
	if (*current.observed != expected) {
		current.state = LuaWorkspaceDocumentState::Conflict;
		current.message = confirmed_target ? "The target changed again during confirmation. Review the current target before retrying." : Difference(baseline, *current.observed);
		return current;
	}
	if (!IsDirty() && !confirmed_target)
		return {};
	if (kind == LuaWorkspaceDocumentKind::KaraokeCode) {
		auto line = context->selectionController->GetDialogueById(dialogue_id);
		auto saved = Snapshot(*line);
		saved.text = persisted;
		if (line->Text.get() != persisted) {
			line->Text = persisted;
			context->ass->Commit("Apply Lua Workspace source", AssFile::COMMIT_DIAG_TEXT, -1, line);
		}
		baseline = std::move(saved);
	}
	else {
		try {
			if (current.observed->text != persisted) {
				agi::io::Save output(filename, true);
				try {
					output.Get().write(persisted.data(), static_cast<std::streamsize>(persisted.size()));
					if (!output.Get().good())
						throw agi::io::IOError("Unable to write the complete Lua source file");
					if (ReadBytes(filename) != current.observed->text) {
						output.Cancel();
						return {.state = LuaWorkspaceDocumentState::Conflict, .message = "The file changed while preparing the save. Review it before retrying."};
					}
					output.Close();
				}
				catch (...) {
					output.Cancel();
					throw;
				}
			}
			baseline = {.text = std::move(persisted)};
		}
		catch (agi::Exception const& error) {
			return Error(error.GetMessage());
		}
		catch (std::exception const& error) {
			return Error(error.what());
		}
	}
	clean_source = source;
	return {};
}
}
