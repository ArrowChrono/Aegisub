#pragma once

#include "lua_source_tools.h"

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class AssDialogue;
namespace agi {
struct Context;
}

namespace Automation4 {
enum class LuaWorkspaceDocumentKind : std::uint8_t { KaraokeCode,
													 LuaFile };
enum class LuaWorkspaceDocumentState : std::uint8_t { Ready,
													  Conflict,
													  Invalidated,
													  Error };

struct LuaWorkspaceSnapshot {
	std::string text;
	bool comment = false;
	std::string effect;
	std::string style;
	bool operator==(LuaWorkspaceSnapshot const&) const = default;
};

struct LuaWorkspaceDocumentResult {
	LuaWorkspaceDocumentState state = LuaWorkspaceDocumentState::Ready;
	std::string message;
	std::optional<LuaSourceDiagnostic> diagnostic;
	std::optional<LuaWorkspaceSnapshot> observed;
	bool Succeeded() const { return state == LuaWorkspaceDocumentState::Ready; }
};

class LuaWorkspaceDocument {
	agi::Context *context = nullptr;
	LuaWorkspaceDocumentKind kind;
	agi::fs::path filename;
	std::uint64_t document_generation = 0;
	int dialogue_id = -1;
	std::string display_name;
	std::string source_identity;
	std::string source;
	std::string clean_source;
	LuaWorkspaceSnapshot baseline;
	std::uint64_t revision = 0;
	bool bom = false;

	explicit LuaWorkspaceDocument(LuaWorkspaceDocumentKind kind);
	LuaWorkspaceDocumentResult ReadCurrent() const;
	LuaWorkspaceDocumentResult LoadSnapshot(LuaWorkspaceSnapshot snapshot);

	public:
	static std::unique_ptr<LuaWorkspaceDocument> OpenCode(agi::Context *context, int dialogue_id, LuaWorkspaceDocumentResult& result);
	static std::unique_ptr<LuaWorkspaceDocument> OpenFile(agi::fs::path const& filename, LuaWorkspaceDocumentResult& result);
	LuaWorkspaceDocumentKind GetKind() const { return kind; }
	std::string const& GetSource() const { return source; }
	std::string const& GetDisplayName() const { return display_name; }
	std::string const& GetSourceIdentity() const { return source_identity; }
	std::uint64_t GetDocumentGeneration() const { return document_generation; }
	int GetDialogueId() const { return dialogue_id; }
	agi::fs::path const& GetFilename() const { return filename; }
	std::uint64_t GetRevision() const { return revision; }
	unsigned GetCodeScopes() const;
	bool IsDirty() const { return source != clean_source; }
	void SetSource(std::string value);
	void DiscardChanges();
	void DetachContext() { context = nullptr; }
	LuaWorkspaceDocumentResult Check() const;
	LuaWorkspaceDocumentResult Reload();
	LuaWorkspaceDocumentResult Save(LuaWorkspaceSnapshot const *confirmed_target = nullptr);
};
}
