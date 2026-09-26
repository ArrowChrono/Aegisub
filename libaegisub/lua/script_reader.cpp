// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "libaegisub/lua/script_reader.h"

#include "libaegisub/file_mapping.h"
#include "libaegisub/log.h"
#include "libaegisub/path.h"
#include "libaegisub/lua/utils.h"
#include "libaegisub/split.h"
#include "libaegisub/string_utils.h"

#include <lauxlib.h>

namespace agi { namespace lua {
namespace {
constexpr char source_observer_key[] = "aegisub.script_source_observer";
constexpr char source_observer_metatable[] = "aegisub.script_source_observer.metatable";

int destroy_source_observer(lua_State *L) {
	static_cast<ScriptSourceObserver *>(lua_touserdata(L, 1))->~ScriptSourceObserver();
	return 0;
}

void notify_source_observer(lua_State *L, fs::path const& filename, std::string_view source) {
	lua_getfield(L, LUA_REGISTRYINDEX, source_observer_key);
	auto *observer = lua_isuserdata(L, -1) ? static_cast<ScriptSourceObserver *>(lua_touserdata(L, -1)) : nullptr;
	lua_pop(L, 1);
	if (observer)
		(*observer)(filename, source);
}
}

void SetScriptSourceObserver(lua_State *L, ScriptSourceObserver observer) {
	if (observer) {
		auto *storage = static_cast<ScriptSourceObserver *>(lua_newuserdata(L, sizeof(ScriptSourceObserver)));
		new (storage) ScriptSourceObserver(std::move(observer));
		if (luaL_newmetatable(L, source_observer_metatable)) {
			lua_pushcfunction(L, destroy_source_observer);
			lua_setfield(L, -2, "__gc");
		}
		lua_setmetatable(L, -2);
	}
	else
		lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, source_observer_key);
}

	static bool ensure_moonscript_loader(lua_State *L) {
		lua_getfield(L, LUA_REGISTRYINDEX, "moonscript");
		if (lua_isfunction(L, -1))
			return true;

		lua_pop(L, 1);
		luaL_loadstring(L, "return require('moonscript').loadstring");
		if (lua_pcall(L, 0, 1, 0))
			return false; // leave error message
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, "moonscript");
		return true;
	}

	bool LoadFile(lua_State *L, agi::fs::path const& raw_filename) {
		auto filename = raw_filename;
		try {
			filename = agi::fs::Canonicalize(raw_filename);
		}
		catch (agi::fs::FileSystemUnknownError const& e) {
			LOG_E("auto4/lua") << "Error canonicalizing path: " << e.GetMessage();
		}

		agi::read_file_mapping file(filename);
		auto buff = file.read();
		size_t size = static_cast<size_t>(file.size());

		// Discard the BOM if present
		if (size >= 3 && buff[0] == -17 && buff[1] == -69 && buff[2] == -65) {
			buff += 3;
			size -= 3;
		}

		auto const filename_utf8 = agi::fs::PathToString(filename);
		if (!agi::fs::HasExtension(filename, "moon")) {
			if (luaL_loadbuffer(L, buff, size, filename_utf8.c_str()) != 0)
				return false;
			notify_source_observer(L, filename, {buff, size});
			return true;
		}

		// We have a MoonScript file, so we need to load it with that
		// It might be nice to have a dedicated lua state for compiling
		// MoonScript to Lua
		if (!ensure_moonscript_loader(L))
			return false;

		// Save the text we'll be loading for the line number rewriting in the
		// error handling
		lua_pushlstring(L, buff, size);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, ("raw moonscript: " + filename_utf8).c_str());

		push_value(L, filename);
		if (lua_pcall(L, 2, 2, 0))
			return false; // Leaves error message on stack

		// loadstring returns nil, error on error or a function on success
		if (lua_isnil(L, 1)) {
			lua_remove(L, 1);
			return false;
		}

		lua_pop(L, 1); // Remove the extra nil for the stackchecker
		notify_source_observer(L, filename, {buff, size});
		return true;
	}

	static int module_loader(lua_State *L) {
		int pretop = lua_gettop(L);
		std::string module(check_string(L, -1));
		agi::util::strings::replace_all_inplace(module, ".", LUA_DIRSEP);

		// Get the lua package include path (which the user may have modified)
		lua_getglobal(L, "package");
		lua_getfield(L, -1, "path");
		std::string package_paths(check_string(L, -1));
		lua_pop(L, 2);

		for (auto tok : agi::Split(package_paths, ';')) {
			std::string filename = agi::util::strings::replace_all_copy(agi::str(tok), "?", module);

			// If there's a .moon file at that path, load it instead of the
			// .lua file
			agi::fs::path path = agi::fs::PathFromString(filename);
			if (agi::fs::HasExtension(path, "lua")) {
				agi::fs::path moonpath = path;
				moonpath.replace_extension("moon");
				if (agi::fs::FileExists(moonpath))
					path = moonpath;
			}

			if (!agi::fs::FileExists(path))
				continue;

			try {
				auto const path_utf8 = agi::fs::PathToString(path);
				if (!LoadFile(L, path))
					return error(L, "Error loading Lua module \"%s\":\n%s", path_utf8.c_str(), check_string(L, 1).c_str());
				break;
			}
			catch (agi::fs::FileNotFound const&) {
				// Not an error so swallow and continue on
			}
			catch (agi::fs::NotAFile const&) {
				// Not an error so swallow and continue on
			}
			catch (agi::Exception const& e) {
				auto const path_utf8 = agi::fs::PathToString(path);
				return error(L, "Error loading Lua module \"%s\":\n%s", path_utf8.c_str(), e.GetMessage().c_str());
			}
		}

		return lua_gettop(L) - pretop;
	}

#ifdef _WIN32
	static void prepend_runtime_cpath(lua_State *L) {
		agi::Path path;

		lua_getglobal(L, "package");
		lua_getfield(L, -1, "cpath");
		std::string cpath = check_string(L, -1);
		lua_pop(L, 1);

		std::string runtime_cpath;
		for (auto const* token : { "?user/runtimes", "?data/runtimes" }) {
			auto runtime_dir = agi::fs::PathToString(path.Decode(token));
			if (runtime_dir.empty())
				continue;
			runtime_cpath += runtime_dir;
			runtime_cpath += "/?.dll;";
		}

		push_value(L, runtime_cpath + cpath);
		lua_setfield(L, -2, "cpath");
		lua_pop(L, 1);
	}

	static bool install_ffi_load_wrapper(lua_State *L) {
		static char const* wrapper = R"lua(
local ffi = require('ffi')
if not rawget(ffi, '__aegisub_original_load') then
  local function has_explicit_path(name)
    return name:find('[\\/]') ~= nil or name:match('^%a:')
  end

  local function is_file(path)
    local lfs = rawget(ffi, '__aegisub_lfs')
    if lfs == nil then
      local ok, loaded = pcall(require, 'lfs')
      lfs = ok and loaded or false
      rawset(ffi, '__aegisub_lfs', lfs)
    end
    return lfs and lfs.attributes(path, 'mode') == 'file'
  end

  local function resolve_library(name)
    if type(name) ~= 'string' or name == '' or has_explicit_path(name) then
      return name
    end

    local candidates = { name }
    if name:lower():sub(-4) == '.dll' then
      candidates = { name:sub(1, -5), name }
    end

    local cpath = package.cpath
    if type(cpath) ~= 'string' then
      return name
    end

    for _, candidate in ipairs(candidates) do
      for template in cpath:gmatch('[^;]+') do
        if template:find('?', 1, true) then
          local path = template:gsub('%?', candidate, 1)
          if is_file(path) then
            return path
          end
        end
      end
    end

    return name
  end

  rawset(ffi, '__aegisub_original_load', ffi.load)
  ffi.load = function(name, global)
    return rawget(ffi, '__aegisub_original_load')(resolve_library(name), global)
  end
end
)lua";

		return luaL_dostring(L, wrapper) == 0;
	}
#endif

	bool Install(lua_State *L, std::vector<fs::path> const& include_path) {
		// set the module load path to include_path
		lua_getglobal(L, "package");
		push_value(L, "path");

		push_value(L, "");
		for (auto const& path : include_path) {
			auto utf8 = agi::fs::PathToString(path);
			lua_pushfstring(L, "%s/?.lua;%s/?/init.lua;", utf8.c_str(), utf8.c_str());
			lua_concat(L, 2);
		}

#ifndef _WIN32
		// No point in checking any of the default locations on Windows since
		// there won't be anything there
		push_value(L, "path");
		lua_gettable(L, -4);
		lua_concat(L, 2);
#endif

		lua_settable(L, -3);

#ifdef _WIN32
		prepend_runtime_cpath(L);
#endif

		// Replace the default lua module loader with our unicode compatible one
		lua_getfield(L, -1, "loaders");
		push_value(L, exception_wrapper<module_loader>);
		lua_rawseti(L, -2, 2);
		lua_pop(L, 2); // loaders, package

#ifdef _WIN32
		if (!install_ffi_load_wrapper(L))
			return false;
#endif

		return true;
	}
} }
