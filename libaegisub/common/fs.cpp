// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "libaegisub/fs.h"
#include "libaegisub/fs_native.h"

#include "libaegisub/access.h"
#include "libaegisub/log.h"
#include "libaegisub/string_utils.h"

#include <filesystem>
#include <random>

namespace bfs = std::filesystem;

// filesystem functions throw a single exception type for all
// errors, which isn't really what we want, so do some crazy wrapper
// shit to map error codes to more useful exceptions.
#ifdef _WIN32
#include "libaegisub/charset_conv_win.h"
#include <winerror.h>
#define CHECKED_CALL(exp, src_path, dst_path) \
	std::error_code ec; \
	exp; \
	switch (ec.value()) {\
		case ERROR_SUCCESS: break; \
		case ERROR_FILE_NOT_FOUND: throw FileNotFound(src_path); \
		case ERROR_DIRECTORY: throw NotADirectory(src_path); \
		case ERROR_DISK_FULL: throw DriveFull(dst_path); \
		case ERROR_ACCESS_DENIED: \
			if (!src_path.empty()) \
				acs::CheckFileRead(src_path); \
			if (!dst_path.empty()) \
				acs::CheckFileWrite(dst_path); \
			throw AccessDenied(src_path); \
		default: \
			LOG_D("filesystem") << "Unknown error when calling '" << #exp << "': " << ec << ": " << ec.message(); \
			throw FileSystemUnknownError(ec.message()); \
	}
#else
#define CHECKED_CALL(exp, src_path, dst_path) \
	std::error_code ec; \
	exp; \
	if (!ec) { \
	} \
	else if (ec == std::errc::no_such_file_or_directory) throw FileNotFound(src_path); \
	else if (ec == std::errc::is_a_directory) throw NotAFile(src_path); \
	else if (ec == std::errc::not_a_directory) throw NotADirectory(src_path); \
	else if (ec == std::errc::no_space_on_device) throw DriveFull(dst_path); \
	else if (ec == std::errc::permission_denied) { \
			if (!src_path.empty()) \
				acs::CheckFileRead(src_path); \
			if (!dst_path.empty()) \
				acs::CheckFileWrite(dst_path); \
			throw AccessDenied(src_path); \
	} \
	else { \
		LOG_D("filesystem") << "Unknown error when calling '" << #exp << "': " << ec << ": " << ec.message(); \
		throw FileSystemUnknownError(ec.message()); \
	}
#endif

#define CHECKED_CALL_RETURN(exp, src_path) \
	CHECKED_CALL(auto ret = exp, src_path, agi::fs::path()); \
	return ret

#define WRAP_BFS(bfs_name, agi_name) \
	auto agi_name(path const& p) -> decltype(bfs::bfs_name(p)) { \
		CHECKED_CALL_RETURN(bfs::bfs_name(p, ec), p); \
	}

#define WRAP_BFS_IGNORE_ERROR(bfs_name, agi_name) \
	auto agi_name(path const& p) -> decltype(bfs::bfs_name(p)) { \
		std::error_code ec; \
		return bfs::bfs_name(p, ec); \
	}

// sasuga windows.h
#undef CreateDirectory

namespace agi { namespace fs {
namespace {
	void ThrowFileSystemError(std::error_code const& ec, path const& src_path, path const& dst_path) {
#ifdef _WIN32
		switch (ec.value()) {
			case ERROR_SUCCESS: return;
			case ERROR_FILE_NOT_FOUND: throw FileNotFound(src_path);
			case ERROR_PATH_NOT_FOUND: throw FileNotFound(src_path);
			case ERROR_DIRECTORY: throw NotADirectory(src_path);
			case ERROR_DISK_FULL: throw DriveFull(dst_path);
			case ERROR_ACCESS_DENIED:
				if (!src_path.empty())
					acs::CheckFileRead(src_path);
				if (!dst_path.empty())
					acs::CheckFileWrite(dst_path);
				throw AccessDenied(src_path);
			default:
				LOG_D("filesystem") << "Unknown error: " << ec << ": " << ec.message();
				throw FileSystemUnknownError(ec.message());
		}
#else
		if (!ec)
			return;
		if (ec == std::errc::no_such_file_or_directory) throw FileNotFound(src_path);
		if (ec == std::errc::is_a_directory) throw NotAFile(src_path);
		if (ec == std::errc::not_a_directory) throw NotADirectory(src_path);
		if (ec == std::errc::no_space_on_device) throw DriveFull(dst_path);
		if (ec == std::errc::permission_denied) {
			if (!src_path.empty())
				acs::CheckFileRead(src_path);
			if (!dst_path.empty())
				acs::CheckFileWrite(dst_path);
			throw AccessDenied(src_path);
		}
		LOG_D("filesystem") << "Unknown error: " << ec << ": " << ec.message();
		throw FileSystemUnknownError(ec.message());
#endif
	}

	WRAP_BFS(file_size, SizeImpl)
	WRAP_BFS(space, Space)

	detail::FileInfo GetFileInfoOrThrow(path const& p) {
		detail::FileInfo info;
		std::error_code ec;
		if (!detail::TryGetFileInfo(p, info, ec))
			ThrowFileSystemError(ec, p, agi::fs::path());
		return info;
	}
}

	std::string PathToString(path const& value) {
#ifdef _WIN32
		return charset::ConvertW(value.native());
#else
		return value.string();
#endif
	}

	std::string PathToGenericString(path const& value) {
#ifdef _WIN32
		return charset::ConvertW(value.generic_wstring());
#else
		return value.generic_string();
#endif
	}

	path PathFromString(std::string const& value) {
#ifdef _WIN32
		return path(charset::ConvertW(value));
#else
		return path(value);
#endif
	}

	path PathFromString(char const* value) {
		return PathFromString(value ? std::string(value) : std::string());
	}

	WRAP_BFS(create_directories, CreateDirectory)
	WRAP_BFS(remove, Remove)
	WRAP_BFS(canonical, Canonicalize)

	bool Exists(path const& p) {
		detail::FileInfo info;
		std::error_code ec;
		return detail::TryGetFileInfo(p, info, ec);
	}

	bool FileExists(path const& p) {
		detail::FileInfo info;
		std::error_code ec;
		return detail::TryGetFileInfo(p, info, ec) && info.type == detail::FileEntryType::regular;
	}

	bool DirectoryExists(path const& p) {
		detail::FileInfo info;
		std::error_code ec;
		return detail::TryGetFileInfo(p, info, ec) && info.type == detail::FileEntryType::directory;
	}

	uintmax_t Size(path const& p) {
		auto const info = GetFileInfoOrThrow(p);
		if (info.type == detail::FileEntryType::directory || info.type == detail::FileEntryType::other)
			throw NotAFile(p);
		return info.size;
	}

	uintmax_t FreeSpace(path const& p) {
		return Space(p).available;
	}

	time_t ModifiedTime(path const& p) {
		return GetFileInfoOrThrow(p).modified_time;
	}

	void Rename(const path& from, const path& to) {
		CHECKED_CALL(detail::Rename(from, to, ec), from, to);
	}

	path UniquePath(path const& model) {
		static thread_local std::mt19937_64 rng(std::random_device{}());
		static constexpr char hex[] = "0123456789abcdef";
		auto result = model.native();
		for (auto& ch : result) {
			if (ch == path::value_type('%'))
				ch = static_cast<path::value_type>(hex[rng() & 0xF]);
		}
		return path(result);
	}

	bool HasExtension(path const& p, std::string const& ext) {
		// Match the C-string semantics used by the std::filesystem operations
		// which consume this path. Embedded NULs are not valid filesystem names,
		// but can still occur in an in-memory path supplied by a caller.
		std::string filename = PathToString(p.filename()).c_str();
		if (filename.size() < ext.size() + 1) return false;
		if (filename[filename.size() - ext.size() - 1] != '.') return false;
		return agi::util::strings::iends_with(filename, ext);
	}
} }
