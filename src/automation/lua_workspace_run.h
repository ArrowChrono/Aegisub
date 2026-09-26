#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Automation4 {
class AutomationDebugSession;

struct LuaWorkspaceSource {
	std::string uri;
	std::string source_identity;
	std::uint64_t revision = 0;
	std::string display_name;
	std::string text;
	bool operator==(LuaWorkspaceSource const&) const = default;
};

inline std::string MakeLuaWorkspaceSourceUri(std::uint64_t invocation_id, std::string_view source_identity, std::uint64_t revision) {
	constexpr char hex[] = "0123456789ABCDEF";
	std::string result = "aegisub-workspace://" + std::to_string(invocation_id) + "/";
	for (unsigned char ch : source_identity) {
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
			result += static_cast<char>(ch);
		else {
			result += '%';
			result += hex[ch >> 4];
			result += hex[ch & 15];
		}
	}
	return result + "/" + std::to_string(revision);
}

struct LuaWorkspaceSourceOverride {
	std::uint64_t document_generation = 0;
	int dialogue_id = -1;
	LuaWorkspaceSource source;
};

class LuaWorkspaceSourceRegistry {
	mutable std::mutex mutex;
	std::map<std::string, std::shared_ptr<LuaWorkspaceSource const>, std::less<>> sources;

	static std::string Normalize(std::string_view uri) {
		if (!uri.empty() && (uri.front() == '=' || uri.front() == '@'))
			uri.remove_prefix(1);
		std::string result(uri);
		std::ranges::replace(result, '\\', '/');
		return result;
	}

	public:
	std::shared_ptr<LuaWorkspaceSource const> Register(LuaWorkspaceSource source) {
		source.uri = Normalize(source.uri);
		std::scoped_lock lock(mutex);
		if (auto found = sources.find(source.uri); found != sources.end()) {
			if (*found->second != source)
				throw std::logic_error("Workspace source identity reused for different compiled source");
			return found->second;
		}
		auto value = std::make_shared<LuaWorkspaceSource const>(std::move(source));
		sources.emplace(value->uri, value);
		return value;
	}

	[[nodiscard]] std::shared_ptr<LuaWorkspaceSource const> Find(std::string_view uri) const {
		auto key = Normalize(uri);
		std::scoped_lock lock(mutex);
		auto found = sources.find(key);
		return found == sources.end() ? nullptr : found->second;
	}
};

struct LuaWorkspaceRunRequest {
	std::uint64_t invocation_id = 0;
	std::string macro_command;
	std::optional<LuaWorkspaceSourceOverride> source_override;
	std::optional<LuaWorkspaceSource> file_source;
	std::shared_ptr<LuaWorkspaceSourceRegistry> sources;
	std::shared_ptr<std::atomic<bool>> stop_requested;
	std::shared_ptr<AutomationDebugSession> debug_session;
};
}
