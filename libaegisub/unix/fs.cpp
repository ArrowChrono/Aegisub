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

#include "libaegisub/access.h"
#include "libaegisub/fs.h"
#include "libaegisub/fs_native.h"
#include "libaegisub/io.h"

#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <fnmatch.h>
#include <istream>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace bfs = std::filesystem;

namespace agi { namespace fs {
namespace {
std::string TrimQueryPath(path const& p) {
	auto native = p.native();
	auto const root_len = p.root_path().native().size();
	while (native.size() > root_len) {
		auto const ch = native.back();
		if (ch != '/' && ch != '\\')
			break;
		native.pop_back();
	}
	return native;
}
}

namespace detail {
bool TryGetFileInfo(path const& p, FileInfo& out, std::error_code& ec) {
	out = {};
	ec.clear();

	auto const query = TrimQueryPath(p);
	if (query.empty()) {
		ec = std::make_error_code(std::errc::no_such_file_or_directory);
		return false;
	}

	struct stat st;
	if (stat(query.c_str(), &st) != 0) {
		ec = std::error_code(errno, std::generic_category());
		return false;
	}

	if (S_ISREG(st.st_mode))
		out.type = FileEntryType::regular;
	else if (S_ISDIR(st.st_mode))
		out.type = FileEntryType::directory;
	else
		out.type = FileEntryType::other;
	out.size = static_cast<uintmax_t>(st.st_size);
	out.modified_time = st.st_mtime;
	return true;
}

void Rename(path const& from, path const& to, std::error_code& ec) {
	bfs::rename(from, to, ec);
}
}

std::string ShortName(path const& p) {
	return PathToString(p);
}

void Touch(path const& file) {
	CreateDirectory(file.parent_path());

	int fd = open(file.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
	if (fd >= 0) {
		futimes(fd, nullptr);
		close(fd);
	}
}

void Copy(fs::path const& from, fs::path const& to) {
	acs::CheckFileRead(from);
	CreateDirectory(to.parent_path());
	acs::CheckDirWrite(to.parent_path());

	auto in = io::Open(from, true);
	io::Save(to).Get() << in->rdbuf();
}

struct DirectoryIterator::PrivData {
	std::error_code ec;
	bfs::directory_iterator it;
	std::string filter;
	PrivData(path const& p, std::string const& filter) : it(p, ec), filter(filter) { }

	bool bad() const {
		return
			it == bfs::directory_iterator() ||
			(!filter.empty() && fnmatch(filter.c_str(), it->path().filename().c_str(), 0));
	}
};

DirectoryIterator::DirectoryIterator() { }
DirectoryIterator::DirectoryIterator(path const& p, std::string const& filter)
: privdata(new PrivData(p, filter))
{
	if (privdata->it == bfs::directory_iterator())
		privdata.reset();
	else if (privdata->bad())
		++*this;
	else
		value = PathToString(privdata->it->path().filename());
}

bool DirectoryIterator::operator==(DirectoryIterator const& rhs) const {
	return privdata.get() == rhs.privdata.get();
}

DirectoryIterator& DirectoryIterator::operator++() {
	if (!privdata) return *this;

	++privdata->it;

	while (privdata->bad()) {
		if (privdata->it == bfs::directory_iterator()) {
			privdata.reset();
			return *this;
		}
		++privdata->it;
	}

	value = PathToString(privdata->it->path().filename());

	return *this;
}

DirectoryIterator::~DirectoryIterator() { }

} }
