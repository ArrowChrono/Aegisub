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

#include <libaegisub/access.h>
#include <libaegisub/file_mapping.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
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

#ifdef _WIN32
namespace {
struct LongWindowsTestDirectory {
	std::filesystem::path root = agi::fs::UniquePath("data/io-long-%%%%%%%%");
	std::filesystem::path native_root = std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(root).make_preferred().native());
	std::filesystem::path directory = root;
	std::filesystem::path absolute_directory;

	LongWindowsTestDirectory() {
		while (std::filesystem::absolute(directory).native().size() < 200)
			directory /= std::string(32, 'd');
		std::filesystem::create_directories(directory);
		absolute_directory = std::filesystem::absolute(directory).make_preferred();
	}
	~LongWindowsTestDirectory() { std::filesystem::remove_all(native_root); }

	[[nodiscard]] std::filesystem::path Target(size_t length, std::filesystem::path const& suffix = ".ass") const {
		return absolute_directory / (std::wstring(length - absolute_directory.native().size() - suffix.native().size() - 1, L'f') + suffix.native());
	}
};
}

TEST(lagi_io, save_close_publishes_long_windows_paths) {
	LongWindowsTestDirectory directory;
	for (size_t length : {259U, 260U, 300U}) {
		SCOPED_TRACE(length);
		auto const absolute_target = directory.Target(length);
		ASSERT_EQ(length, absolute_target.native().size());
		auto const native_target = std::filesystem::path(L"\\\\?\\" + absolute_target.native());
		std::ofstream(native_target, std::ios::binary) << "original";
		ASSERT_EQ("original", read_all(native_target));

		for (auto const& target : {directory.directory / absolute_target.filename(), absolute_target}) {
			agi::io::Save save(target, true);
			save.Get() << "replacement subtitle contents";
			EXPECT_EQ("original", read_all(native_target));
			ASSERT_NO_THROW(save.Close());
			EXPECT_EQ("replacement subtitle contents", read_all(native_target));
			EXPECT_TRUE(list_matching(directory.directory, "*_tmp_*.ass").empty());
			std::ofstream(native_target, std::ios::binary) << "original";
		}
	}
}

TEST(lagi_io, file_streams_report_invalid_paths_with_failbit) {
	std::ifstream input;
	EXPECT_NO_THROW(agi::io::OpenFileStream(input, {}));
	EXPECT_TRUE(input.fail());
	std::ofstream output;
	EXPECT_NO_THROW(agi::io::OpenFileStream(output, {}));
	EXPECT_TRUE(output.fail());

	std::ifstream throwing_input;
	throwing_input.exceptions(std::ios::failbit);
	EXPECT_THROW(agi::io::OpenFileStream(throwing_input, {}), std::ios_base::failure);
	std::ofstream throwing_output;
	throwing_output.exceptions(std::ios::failbit);
	EXPECT_THROW(agi::io::OpenFileStream(throwing_output, {}), std::ios_base::failure);
}
TEST(lagi_io, file_streams_round_trip_long_windows_paths) {
	LongWindowsTestDirectory directory;
	for (size_t length : {259U, 260U, 300U}) {
		SCOPED_TRACE(length);
		auto const absolute_target = directory.Target(length);
		ASSERT_EQ(length, absolute_target.native().size());
		auto const native_target = std::filesystem::path(L"\\\\?\\" + absolute_target.native());
		for (auto const& target : {directory.directory / absolute_target.filename(), absolute_target, native_target}) {
			std::ofstream(native_target, std::ios::binary) << "seed value";
			ASSERT_EQ("seed value", read_all(native_target));
			EXPECT_NO_THROW({
				auto existing_input = agi::io::Open(target, true);
				EXPECT_EQ("seed value", std::string(std::istreambuf_iterator<char>(*existing_input), {}));
			});
			auto output = agi::io::OpenOutputFileStream(target, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(output.good());
			output << "first line\nsecond line";
			output.close();
			ASSERT_TRUE(output.good());
			EXPECT_EQ("first line\nsecond line", read_all(native_target));

			auto append = agi::io::OpenOutputFileStream(target, std::ios::binary | std::ios::app);
			ASSERT_TRUE(append.good());
			append << "\nappended";
			append.close();
			ASSERT_TRUE(append.good());
			auto input = agi::io::OpenInputFileStream(target, std::ios::binary);
			ASSERT_TRUE(input.good());
			EXPECT_EQ("first line\nsecond line\nappended", std::string(std::istreambuf_iterator<char>(input), {}));
			auto high_level_input = agi::io::Open(target, true);
			EXPECT_EQ("first line\nsecond line\nappended", std::string(std::istreambuf_iterator<char>(*high_level_input), {}));
		}
	}
}

TEST(lagi_io, long_windows_paths_preserve_file_and_directory_metadata) {
	LongWindowsTestDirectory directory;
	for (size_t length : {259U, 260U, 300U}) {
		SCOPED_TRACE(length);
		auto const absolute_target = directory.Target(length);
		ASSERT_EQ(length, absolute_target.native().size());
		auto const native_target = std::filesystem::path(L"\\\\?\\" + absolute_target.native());
		std::ofstream(native_target, std::ios::binary) << "existing file";
		ASSERT_EQ("existing file", read_all(native_target));
		auto const absolute_folder = std::filesystem::path(absolute_target).replace_extension(".dir");
		auto const native_folder = std::filesystem::path(L"\\\\?\\" + absolute_folder.native());
		ASSERT_TRUE(std::filesystem::create_directory(native_folder));
		auto const expected_time = agi::fs::ModifiedTime(native_target);

		for (auto const& target : {directory.directory / absolute_target.filename(), absolute_target, native_target}) {
			EXPECT_TRUE(agi::fs::Exists(target));
			EXPECT_TRUE(agi::fs::FileExists(target));
			EXPECT_FALSE(agi::fs::DirectoryExists(target));
			EXPECT_EQ(13U, agi::fs::Size(target));
			EXPECT_EQ(expected_time, agi::fs::ModifiedTime(target));
			auto folder = std::filesystem::path(target).replace_extension(".dir");
			EXPECT_TRUE(agi::fs::Exists(folder));
			EXPECT_TRUE(agi::fs::DirectoryExists(folder));
			EXPECT_TRUE(agi::fs::DirectoryExists(folder / ""));
			EXPECT_FALSE(agi::fs::FileExists(folder));
			EXPECT_THROW(agi::fs::Size(folder), agi::fs::NotAFile);
			auto const missing = target.parent_path() / (target.filename().native() + L".missing");
			EXPECT_FALSE(agi::fs::Exists(missing));
			EXPECT_FALSE(agi::fs::FileExists(missing));
			EXPECT_THROW(agi::fs::Size(missing), agi::fs::FileNotFound);
		}
	}
}

TEST(lagi_io, read_file_mapping_reads_long_windows_paths) {
	LongWindowsTestDirectory directory;
	auto const suffix = agi::fs::PathFromString("-\xE5\xAD\x97\xE5\xB9\x95\xF0\x9F\x98\x80.ass");
	auto const contents = std::string("header\r\n") + std::string(4096, 'x') + std::string("\0tail\r\n", 7);
	for (size_t length : {259U, 260U, 262U, 300U}) {
		SCOPED_TRACE(length);
		auto const absolute_target = directory.Target(length, suffix);
		ASSERT_EQ(length, absolute_target.native().size());
		auto const native_target = std::filesystem::path(L"\\\\?\\" + absolute_target.native());
		std::ofstream(native_target, std::ios::binary) << contents;
		ASSERT_EQ(contents, read_all(native_target));

		size_t form = 0;
		for (auto const& target : {directory.directory / absolute_target.filename(), absolute_target, native_target}) {
			SCOPED_TRACE(form++);
			EXPECT_NO_THROW({
				agi::read_file_mapping input(target);
				EXPECT_EQ(contents.size(), input.size());
				EXPECT_EQ(contents, std::string(input.read(), static_cast<size_t>(input.size())));
			});
			EXPECT_EQ(contents, read_all(native_target));
		}
	}
}

TEST(lagi_io, temp_file_mapping_writes_and_removes_long_windows_paths) {
	LongWindowsTestDirectory directory;
	auto const suffix = agi::fs::PathFromString("-\xE5\xAD\x97\xE5\xB9\x95\xF0\x9F\x98\x80.tmp");
	auto const absolute_target = directory.Target(300, suffix);
	ASSERT_EQ(300U, absolute_target.native().size());
	auto const native_target = std::filesystem::path(L"\\\\?\\" + absolute_target.native());
	auto const contents = std::string("mapped\0bytes\r\n", 14) + std::string(4096, 't');
	size_t form = 0;
	for (auto const& target : {directory.directory / absolute_target.filename(), absolute_target, native_target}) {
		SCOPED_TRACE(form++);
		EXPECT_NO_THROW({
			agi::temp_file_mapping mapping(target, contents.size());
			std::ranges::copy(contents, mapping.write(0, contents.size()));
			EXPECT_EQ(contents, std::string(mapping.read(0, contents.size()), contents.size()));
			EXPECT_EQ(contents.size(), std::filesystem::file_size(native_target));
		});
		EXPECT_FALSE(std::filesystem::exists(native_target));
	}
}

TEST(lagi_io, copy_long_windows_paths_preserves_bytes_and_overwrites) {
	LongWindowsTestDirectory directory;
	auto const suffix = agi::fs::PathFromString("-\xE5\xAD\x97\xE5\xB9\x95\xF0\x9F\x98\x80.ass");
	auto const absolute_root = std::filesystem::absolute(directory.root).make_preferred();
	struct CopyPaths {
		char const *name;
		std::filesystem::path source;
		std::filesystem::path destination;
	};
	CopyPaths const cases[] = {
		{.name = "short_to_boundary_259", .source = absolute_root / "source.ass", .destination = directory.Target(259, suffix)},
		{.name = "short_to_boundary_260", .source = absolute_root / "source.ass", .destination = directory.Target(260, suffix)},
		{.name = "long_to_short", .source = directory.Target(260, suffix), .destination = absolute_root / "destination.ass"},
		{.name = "long_to_long", .source = directory.Target(262, suffix), .destination = directory.Target(271, suffix)},
		{.name = "long_parent", .source = directory.Target(262, suffix), .destination = directory.Target(300, ".dir") / suffix}};
	std::string const contents("subtitle\0copied\r\n", 17);
	std::string const original = "original destination content must be replaced and truncated";
	for (auto const& paths : cases) {
		SCOPED_TRACE(paths.name);
		auto const native_source = std::filesystem::path(L"\\\\?\\" + paths.source.native());
		auto const native_destination = std::filesystem::path(L"\\\\?\\" + paths.destination.native());
		std::filesystem::create_directories(native_destination.parent_path());
		std::ofstream(native_source, std::ios::binary) << contents;
		ASSERT_EQ(contents, read_all(native_source));

		for (size_t form : {0U, 1U, 2U}) {
			SCOPED_TRACE(form);
			auto const source = form == 0   ? paths.source.lexically_relative(std::filesystem::current_path())
								: form == 1 ? paths.source
											: native_source;
			auto const destination = form == 0   ? paths.destination.lexically_relative(std::filesystem::current_path())
									 : form == 1 ? paths.destination
												 : native_destination;
			EXPECT_NO_THROW(agi::acs::CheckFileRead(source));
			EXPECT_NO_THROW(agi::acs::CheckDirWrite(destination.parent_path()));
			std::filesystem::remove(native_destination);
			EXPECT_NO_THROW(agi::fs::Copy(source, destination));
			EXPECT_EQ(contents, read_all(native_destination));
			EXPECT_EQ(contents, read_all(native_source));

			std::ofstream(native_destination, std::ios::binary) << original;
			ASSERT_EQ(original, read_all(native_destination));
			EXPECT_NO_THROW(agi::fs::Copy(source, destination));
			EXPECT_EQ(contents, read_all(native_destination));
			EXPECT_EQ(contents, read_all(native_source));
		}
	}
}

TEST(lagi_io, long_windows_copy_failures_preserve_destination_and_error_types) {
	LongWindowsTestDirectory directory;
	auto const suffix = agi::fs::PathFromString("-\xE5\xAD\x97\xE5\xB9\x95\xF0\x9F\x98\x80.ass");
	auto const absolute_source = directory.Target(300, suffix);
	auto const native_source = std::filesystem::path(L"\\\\?\\" + absolute_source.native());
	auto const native_folder = std::filesystem::path(native_source).replace_extension(".dir");
	std::filesystem::create_directory(native_folder);
	std::ofstream(native_source, std::ios::binary) << "source contents";
	ASSERT_EQ("source contents", read_all(native_source));
	auto const destination = directory.root / "preserved.ass";
	std::string const original = "existing destination must survive a failed copy";
	std::ofstream(destination, std::ios::binary) << original;
	ASSERT_EQ(original, read_all(destination));
	auto const short_missing = directory.root / "missing.ass";
	EXPECT_THROW({ agi::read_file_mapping input(short_missing); }, agi::fs::FileNotFound);
	EXPECT_THROW(agi::acs::CheckFileRead(short_missing), agi::fs::FileNotFound);
	EXPECT_THROW(agi::fs::Copy(short_missing, destination), agi::fs::FileNotFound);
	EXPECT_EQ(original, read_all(destination));
	EXPECT_THROW({ agi::read_file_mapping input(std::filesystem::path{}); }, agi::fs::FileNotFound);
	EXPECT_THROW(agi::acs::CheckFileRead({}), agi::fs::FileNotFound);
	EXPECT_THROW(agi::fs::Copy({}, destination), agi::fs::FileNotFound);
	EXPECT_EQ(original, read_all(destination));

	size_t form = 0;
	for (auto const& source : {directory.directory / absolute_source.filename(), absolute_source, native_source}) {
		SCOPED_TRACE(form++);
		auto const missing = std::filesystem::path(source).replace_extension(".missing");
		EXPECT_THROW({ agi::read_file_mapping input(missing); }, agi::fs::FileNotFound);
		EXPECT_THROW(agi::acs::CheckFileRead(missing), agi::fs::FileNotFound);
		EXPECT_THROW(agi::fs::Copy(missing, destination), agi::fs::FileNotFound);
		EXPECT_EQ(original, read_all(destination));

		auto const folder = std::filesystem::path(source).replace_extension(".dir");
		EXPECT_THROW(agi::acs::CheckFileRead(folder), agi::fs::NotAFile);
		EXPECT_THROW(agi::fs::Copy(folder, destination), agi::fs::NotAFile);
		EXPECT_EQ(original, read_all(destination));
		EXPECT_THROW(agi::acs::CheckDirWrite(source), agi::fs::NotADirectory);
		EXPECT_EQ("source contents", read_all(native_source));
	}
}
#endif