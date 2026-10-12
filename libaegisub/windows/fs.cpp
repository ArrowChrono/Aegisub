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
#include "libaegisub/charset_conv_win.h"
#include "libaegisub/exception.h"
#include "libaegisub/scoped_ptr.h"
#include "libaegisub/util.h"

using agi::charset::ConvertW;
using agi::charset::ConvertLocal;

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#undef CreateDirectory

namespace agi { namespace fs {
namespace {
std::string ShortNameFallback(path const& p) {
	try {
		return ConvertLocal(p.native());
	}
	catch (agi::Exception const&) {
		return PathToString(p);
	}
	catch (std::exception const&) {
		return PathToString(p);
	}
}

std::wstring TrimQueryPath(path const& p) {
	auto native = p.native();
	if (native.size() >= MAX_PATH && !native.starts_with(L"\\\\?\\") && !native.starts_with(L"\\\\.\\"))
		native = p.lexically_normal().native();
	auto const root_len = p.root_path().native().size();
	while (native.size() > root_len) {
		auto const ch = native.back();
		if (ch != L'\\' && ch != L'/')
			break;
		native.pop_back();
	}
	return native;
}

time_t FileTimeToUnixTime(FILETIME const& ft) {
	ULARGE_INTEGER value;
	value.LowPart = ft.dwLowDateTime;
	value.HighPart = ft.dwHighDateTime;
	if (value.QuadPart < 116444736000000000ULL)
		return 0;
	return static_cast<time_t>((value.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
}

namespace detail {
bool TryGetFileInfo(path const& p, FileInfo& out, std::error_code& ec) {
	out = {};
	ec.clear();

	auto const query = TrimQueryPath(p);
	if (query.empty()) {
		ec = std::error_code(ERROR_FILE_NOT_FOUND, std::system_category());
		return false;
	}

	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(query.c_str(), GetFileExInfoStandard, &data)) {
		ec = std::error_code(GetLastError(), std::system_category());
		return false;
	}

	out.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		? FileEntryType::directory
		: FileEntryType::regular;
	out.size = (static_cast<uintmax_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
	out.modified_time = FileTimeToUnixTime(data.ftLastWriteTime);
	return true;
}
}

std::string ShortName(path const& p) {
	std::wstring out(MAX_PATH + 1, 0);
	DWORD len = GetShortPathNameW(p.c_str(), &out[0], static_cast<DWORD>(out.size()));
	if (!len)
		return ShortNameFallback(p);
	while (len >= out.size()) {
		out.resize(len + 1, 0);
		len = GetShortPathNameW(p.c_str(), &out[0], static_cast<DWORD>(out.size()));
		if (!len)
			return ShortNameFallback(p);
	}
	out.resize(len);
	return ConvertLocal(out);
}

void Touch(path const& file) {
	CreateDirectory(file.parent_path());

	SYSTEMTIME st;
	FILETIME ft;
	GetSystemTime(&st);
	if(!SystemTimeToFileTime(&st, &ft))
		throw EnvironmentError("SystemTimeToFileTime failed with error: " + util::ErrorString(GetLastError()));

	scoped_holder<HANDLE, BOOL (__stdcall *)(HANDLE)>
		h(CreateFile(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr), CloseHandle);
	// error handling etc.
	if (!SetFileTime(h, nullptr, nullptr, &ft))
		throw EnvironmentError("SetFileTime failed with error: " + util::ErrorString(GetLastError()));
}

void Copy(fs::path const& from, fs::path const& to) {
	acs::CheckFileRead(from);
	CreateDirectory(to.parent_path());
	acs::CheckDirWrite(to.parent_path());

	if (!CopyFile(from.wstring().c_str(), to.wstring().c_str(), false)) {
		switch (GetLastError()) {
		case ERROR_FILE_NOT_FOUND:
			throw FileNotFound(from);
		case ERROR_ACCESS_DENIED:
			throw fs::WriteDenied("Could not overwrite " + PathToString(to));
		default:
			throw fs::WriteDenied("Could not copy: " + util::ErrorString(GetLastError()));
		}
	}
}

struct DirectoryIterator::PrivData {
	scoped_holder<HANDLE, BOOL (__stdcall *)(HANDLE)> h{INVALID_HANDLE_VALUE, FindClose};
};

DirectoryIterator::DirectoryIterator() { }
DirectoryIterator::DirectoryIterator(path const& p, std::string const& filter)
: privdata(new PrivData)
{
	WIN32_FIND_DATA data;
	privdata->h = FindFirstFileEx((p/(filter.empty() ? "*.*" : filter)).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
	if (privdata->h == INVALID_HANDLE_VALUE) {
		privdata.reset();
		return;
	}

	value = ConvertW(data.cFileName);
	while (value[0] == '.' && (value[1] == 0 || value[1] == '.'))
		++*this;
}

bool DirectoryIterator::operator==(DirectoryIterator const& rhs) const {
	return privdata.get() == rhs.privdata.get();
}

DirectoryIterator& DirectoryIterator::operator++() {
	WIN32_FIND_DATA data;
	if (FindNextFile(privdata->h, &data))
		value = ConvertW(data.cFileName);
	else {
		privdata.reset();
		value.clear();
	}
	return *this;
}

DirectoryIterator::~DirectoryIterator() { }

} }
