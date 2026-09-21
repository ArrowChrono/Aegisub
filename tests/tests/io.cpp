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

#include <main.h>

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/scope_exit.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string read_all(std::filesystem::path const& path) {
	std::ifstream stream(path, std::ios::binary);
	std::ostringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

std::vector<std::string> list_matching(std::filesystem::path const& dir, std::string const& pattern) {
	std::vector<std::string> files;
	agi::fs::DirectoryIterator(dir, pattern).GetAll(files);
	return files;
}
}

TEST(lagi_io, save_close_overwrites_file_and_cleans_temp_file) {
	auto const target = std::filesystem::path("data/save_close.txt");
	std::filesystem::remove(target);
	{
		std::ofstream seed(target, std::ios::binary);
		seed << "old-value";
	}

	{
		agi::io::Save save(target, true);
		save.Get() << "new-value";
		save.Close();
		save.Close();
	}

	EXPECT_EQ("new-value", read_all(target));
	EXPECT_TRUE(list_matching("data", "save_close_tmp_*.txt").empty());
}

TEST(lagi_io, save_destructor_commits_file_without_leftovers) {
	auto const target = std::filesystem::path("data/save_destructor.txt");
	std::filesystem::remove(target);

	{
		agi::io::Save save(target, true);
		save.Get() << "written-via-destructor";
	}

	EXPECT_TRUE(agi::fs::FileExists(target));
	EXPECT_EQ("written-via-destructor", read_all(target));
	EXPECT_TRUE(list_matching("data", "save_destructor_tmp_*.txt").empty());
}

TEST(lagi_io, unicode_path_survives_logging_and_round_trips) {
	auto const target = std::filesystem::path("data") /
		agi::fs::PathFromString("io-unicode-\xF0\x9F\x98\x80.txt");
	std::filesystem::remove(target);

	{
		agi::io::Save save(target, true);
		save.Get() << "unicode-path-content";
	}

	auto input = agi::io::Open(target, true);
	std::ostringstream contents;
	contents << input->rdbuf();
	EXPECT_EQ("unicode-path-content", contents.str());

	input.reset();
	std::filesystem::remove(target);
}

namespace {
struct SaveTestDirectory {
	std::filesystem::path path = agi::fs::UniquePath(std::filesystem::temp_directory_path() / "aegisub-save-%%%%%%%%");
	SaveTestDirectory() { std::filesystem::create_directory(path); }
	~SaveTestDirectory() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};
}

TEST(lagi_io, save_cancel_preserves_existing_target_and_is_idempotent) {
	SaveTestDirectory directory;
	auto const target = directory.path / "target.txt";
	{
		std::ofstream(target, std::ios::binary) << "original";
	}
	{
		agi::io::Save save(target, true);
		save.Get() << "uncommitted replacement";
		ASSERT_EQ(1U, list_matching(directory.path, "target_tmp_*.txt").size());
		save.Cancel();
		save.Cancel();
		save.Close();
	}
	EXPECT_EQ("original", read_all(target));
	EXPECT_TRUE(list_matching(directory.path, "target_tmp_*.txt").empty());
}

TEST(lagi_io, save_cancel_during_unwind_does_not_create_destination) {
	SaveTestDirectory directory;
	auto const target = directory.path / "target.txt";
	auto fail_after_write = [&] {
		agi::io::Save save(target, true);
		auto cancel = agi::make_scope_exit([&] { save.Cancel(); });
		save.Get() << "incomplete output";
		throw std::runtime_error("injected producer failure");
	};
	EXPECT_THROW(fail_after_write(), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(target));
	EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}

TEST(lagi_io, save_cancel_after_commit_preserves_committed_output) {
	SaveTestDirectory directory;
	auto const target = directory.path / "target.txt";
	{
		agi::io::Save save(target, true);
		save.Get() << "committed";
		save.Close();
		save.Cancel();
		save.Cancel();
	}
	EXPECT_EQ("committed", read_all(target));
	EXPECT_TRUE(list_matching(directory.path, "target_tmp_*.txt").empty());
}

TEST(lagi_io, save_cancel_removes_temp_after_failed_stream_close) {
	SaveTestDirectory directory;
	auto const target = directory.path / "target.txt";
	{
		std::ofstream(target, std::ios::binary) << "original";
	}
	{
		agi::io::Save save(target, true);
		save.Get() << "partial replacement";
		save.Get().setstate(std::ios::badbit);
		EXPECT_THROW(save.Close(), agi::fs::WriteDenied);
		EXPECT_EQ("original", read_all(target));
		ASSERT_EQ(1U, list_matching(directory.path, "target_tmp_*.txt").size());
		save.Cancel();
		save.Cancel();
	}
	EXPECT_EQ("original", read_all(target));
	EXPECT_TRUE(list_matching(directory.path, "target_tmp_*.txt").empty());
}

TEST(lagi_io, save_cancel_removes_closed_temp_after_rename_failure) {
	SaveTestDirectory directory;
	auto const target = directory.path / "target.txt";
	ASSERT_TRUE(std::filesystem::create_directory(target));
	{
		std::ofstream(target / "marker", std::ios::binary) << "preserved";
	}
	{
		agi::io::Save save(target, true);
		save.Get() << "cannot replace a directory";
		EXPECT_THROW(save.Close(), agi::fs::FileSystemError);
		EXPECT_EQ("preserved", read_all(target / "marker"));
		ASSERT_EQ(1U, list_matching(directory.path, "target_tmp_*.txt").size());
		save.Cancel();
	}
	EXPECT_TRUE(std::filesystem::is_directory(target));
	EXPECT_EQ("preserved", read_all(target / "marker"));
	EXPECT_TRUE(list_matching(directory.path, "target_tmp_*.txt").empty());
}
