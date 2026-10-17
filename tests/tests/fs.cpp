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
#include <util.h>

#include <libaegisub/fs.h>
#include <libaegisub/scoped_ptr.h>

#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <Windows.h>
#undef CreateDirectory
#endif
#include <set>

using namespace agi::fs;

TEST(lagi_fs, exists) {
	EXPECT_TRUE(Exists("data/file"));
	EXPECT_TRUE(Exists("data/dir"));
	EXPECT_FALSE(Exists("data/nonexistent"));
}

TEST(lagi_fs, exists_does_not_throw) {
	EXPECT_NO_THROW(Exists("data/file"));
	EXPECT_NO_THROW(Exists("data/dir"));
	EXPECT_NO_THROW(Exists("data/nonexistent"));
	EXPECT_NO_THROW(Exists("data/dir_access_denied"));
	EXPECT_NO_THROW(Exists("data/file_access_denied"));
	EXPECT_NO_THROW(Exists("schema://host/file"));
}

TEST(lagi_fs, file_exists) {
	EXPECT_TRUE(FileExists("data/file"));
	EXPECT_FALSE(FileExists("data/dir"));
}

TEST(lagi_fs, dir_exists) {
	EXPECT_FALSE(DirectoryExists("data/file"));
	EXPECT_TRUE(DirectoryExists("data/dir"));
}

#ifdef _WIN32
TEST(lagi_fs, short_name_fallback_handles_utf8_only_paths) {
	auto const utf8_path = std::string("data/") + "\xF0\x9F\x98\x80" + "-short-name-fallback";
	auto const path = PathFromString(utf8_path);
	auto const expected = PathToString(path);

	EXPECT_NO_THROW({
		auto const short_name = ShortName(path);
		EXPECT_EQ(expected, short_name);
	});
}
#else
TEST(lagi_fs, short_name_is_a_no_op) {
	EXPECT_STREQ("a b c d", ShortName("a b c d").c_str());
}
#endif

TEST(lagi_fs, free_space_returns_a_value) {
	uintmax_t space = 0;
	ASSERT_NO_THROW(space = FreeSpace("."));
	EXPECT_LT(0, space);
}

TEST(lagi_fs, file_size) {
	EXPECT_EQ(0u, Size("data/file"));
	EXPECT_EQ(10u, Size("data/ten_bytes"));
	EXPECT_THROW(Size("data/dir"), NotAFile);
}

TEST(lagi_fs, touch_creates_file) {
	Remove("data/touch_tmp");
	ASSERT_FALSE(Exists("data/touch_tmp"));
	Touch("data/touch_tmp");
	EXPECT_TRUE(Exists("data/touch_tmp"));
	Remove("data/touch_tmp");
}

TEST(lagi_fs, touch_updates_modified_time) {
	time_t mod_time = ModifiedTime("data/touch_mod_time");
	Touch("data/touch_mod_time");
	EXPECT_LT(mod_time, ModifiedTime("data/touch_mod_time"));
}

TEST(lagi_fs, touch_does_not_modify_contents) {
	int expected_value = util::write_rand("data/tmp");

	Touch("data/tmp");

	EXPECT_EQ(expected_value, util::read_written_rand("data/tmp"));
}

TEST(lagi_fs, rename) {
	ASSERT_NO_THROW(Touch("data/rename_in"));
	ASSERT_NO_THROW(Remove("data/rename_out"));
	ASSERT_NO_THROW(Rename("data/rename_in", "data/rename_out"));
	EXPECT_FALSE(FileExists("data/rename_in"));
	EXPECT_TRUE(FileExists("data/rename_out"));
}

TEST(lagi_fs, rename_overwrites) {
	int expected_value = util::write_rand("data/rename_in");
	ASSERT_NO_THROW(Remove("data/rename_out"));
	ASSERT_NO_THROW(Touch("data/rename_out"));

	ASSERT_NO_THROW(Rename("data/rename_in", "data/rename_out"));

	EXPECT_FALSE(FileExists("data/rename_in"));
	EXPECT_TRUE(FileExists("data/rename_out"));
	EXPECT_EQ(expected_value, util::read_written_rand("data/rename_out"));
}

#ifdef _WIN32
namespace {
bool WriteRenameContents(path const& file, char const *contents) {
	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	stream << contents;
	stream.close();
	return stream.good();
}

std::string ReadRenameContents(path const& file) {
	std::ifstream stream(file, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void CheckRenameOpenTarget(DWORD access) {
	auto const source = path("data/rename_open_in");
	auto const target = path("data/rename_open_out");
	std::string const old_contents = "old target";
	std::string const new_contents = "replacement source contents";
	ASSERT_TRUE(WriteRenameContents(source, new_contents.c_str()));
	ASSERT_TRUE(WriteRenameContents(target, old_contents.c_str()));

	auto raw = CreateFileW(target.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						   nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	ASSERT_NE(INVALID_HANDLE_VALUE, raw) << GetLastError();
	agi::scoped_holder<HANDLE, decltype(&CloseHandle)> held_target(raw, CloseHandle);
	BY_HANDLE_FILE_INFORMATION before{};
	ASSERT_TRUE(GetFileInformationByHandle(held_target, &before)) << GetLastError();

	ASSERT_NO_THROW(Rename(source, target));
	EXPECT_FALSE(FileExists(source));
	EXPECT_EQ(new_contents, ReadRenameContents(target));

	BY_HANDLE_FILE_INFORMATION after{};
	ASSERT_TRUE(GetFileInformationByHandle(held_target, &after)) << GetLastError();
	EXPECT_EQ(before.dwVolumeSerialNumber, after.dwVolumeSerialNumber);
	EXPECT_EQ(before.nFileIndexHigh, after.nFileIndexHigh);
	EXPECT_EQ(before.nFileIndexLow, after.nFileIndexLow);
	EXPECT_EQ(0u, after.nFileSizeHigh);
	EXPECT_EQ(old_contents.size(), after.nFileSizeLow);
	if (access & GENERIC_READ) {
		char buffer[64]{};
		DWORD read = 0;
		ASSERT_TRUE(ReadFile(held_target, buffer, sizeof buffer, &read, nullptr)) << GetLastError();
		EXPECT_EQ(old_contents, std::string(buffer, read));
	}
}
}

TEST(lagi_fs, rename_overwrites_metadata_open_target) {
	CheckRenameOpenTarget(FILE_READ_ATTRIBUTES);
}

TEST(lagi_fs, rename_overwrites_read_open_target) {
	CheckRenameOpenTarget(GENERIC_READ);
}

TEST(lagi_fs, rename_preserves_target_without_delete_sharing) {
	auto const source = path("data/rename_no_share_in");
	auto const target = path("data/rename_no_share_out");
	ASSERT_TRUE(WriteRenameContents(source, "new source"));
	ASSERT_TRUE(WriteRenameContents(target, "old target"));
	auto raw = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
						   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(INVALID_HANDLE_VALUE, raw) << GetLastError();
	agi::scoped_holder<HANDLE, decltype(&CloseHandle)> held_target(raw, CloseHandle);

	EXPECT_THROW(Rename(source, target), FileSystemError);
	EXPECT_EQ("new source", ReadRenameContents(source));
	EXPECT_EQ("old target", ReadRenameContents(target));
}

TEST(lagi_fs, rename_preserves_read_only_target) {
	auto const source = path("data/rename_read_only_in");
	auto const target = path("data/rename_read_only_out");
	ASSERT_TRUE(WriteRenameContents(source, "new source"));
	ASSERT_TRUE(WriteRenameContents(target, "old target"));
	ASSERT_TRUE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY)) << GetLastError();

	EXPECT_THROW(Rename(source, target), AccessDenied);
	EXPECT_NE(0u, GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_READONLY);
	EXPECT_EQ("new source", ReadRenameContents(source));
	EXPECT_EQ("old target", ReadRenameContents(target));
	EXPECT_TRUE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL)) << GetLastError();
}
#endif

TEST(lagi_fs, copy) {
	int expected_value = util::write_rand("data/copy_in");
	ASSERT_NO_THROW(Remove("data/copy_out"));

	ASSERT_NO_THROW(Copy("data/copy_in", "data/copy_out"));

	EXPECT_TRUE(FileExists("data/copy_in"));
	EXPECT_TRUE(FileExists("data/copy_out"));
	EXPECT_EQ(expected_value, util::read_written_rand("data/copy_out"));
}

TEST(lagi_fs, copy_overwrites) {
	int expected_value = util::write_rand("data/copy_in");
	ASSERT_NO_THROW(Remove("data/copy_out"));
	ASSERT_NO_THROW(Touch("data/copy_out"));

	ASSERT_NO_THROW(Copy("data/copy_in", "data/copy_out"));

	EXPECT_TRUE(FileExists("data/copy_in"));
	EXPECT_TRUE(FileExists("data/copy_out"));
	EXPECT_EQ(expected_value, util::read_written_rand("data/copy_out"));
}

TEST(lagi_fs, copy_creates_path) {
	int expected_value = util::write_rand("data/copy_in");
	auto const nested_root = std::filesystem::path("data/copy_nested");
	auto const nested_out = nested_root / "dir" / "copy_out";
	std::filesystem::remove_all(nested_root);

	ASSERT_NO_THROW(Copy("data/copy_in", nested_out));

	EXPECT_TRUE(FileExists(nested_out));
	EXPECT_EQ(expected_value, util::read_written_rand(nested_out.string().c_str()));

	std::filesystem::remove_all(nested_root);
}

TEST(lagi_fs, has_extension) {
	EXPECT_TRUE(HasExtension("foo.txt", "txt"));
	EXPECT_TRUE(HasExtension("foo.TXT", "txt"));
	EXPECT_TRUE(HasExtension("foo.tar.gz", "gz"));
	EXPECT_TRUE(HasExtension("foo.tar.gz", "tar.gz"));
	EXPECT_TRUE(HasExtension(PathFromString("foo.\xC3\x9F"), "\xC3\x9F")); // sharp s

	EXPECT_FALSE(HasExtension("foo.tx", "txt"));
	EXPECT_FALSE(HasExtension("footxt", "txt"));
	EXPECT_FALSE(HasExtension("foo.txt/", "txt"));
	EXPECT_FALSE(HasExtension("foo.tar.gz", "tar"));
}

TEST(lagi_fs, has_extension_stops_at_embedded_nul) {
	std::string value("foo.txt\0.bak", 12);
	auto const path = PathFromString(value);

	EXPECT_TRUE(HasExtension(path, "txt"));
	EXPECT_FALSE(HasExtension(path, "bak"));
}

TEST(lagi_fs, create_directory_creates_intermediate_directories) {
	auto const root = std::filesystem::path("data/fs_nested");
	auto const leaf = root / "alpha" / "beta" / "gamma";
	std::filesystem::remove_all(root);

	ASSERT_NO_THROW(CreateDirectory(leaf));
	EXPECT_TRUE(DirectoryExists(leaf));

	std::filesystem::remove_all(root);
}

TEST(lagi_fs, unique_path_replaces_placeholders_and_preserves_shape) {
	path const model = "data/unique_%%%%%%%%.tmp";
	std::set<std::string> seen;

	for (int i = 0; i < 128; ++i) {
		auto generated = UniquePath(model);
		auto const generated_str = generated.string();
		EXPECT_EQ(std::string::npos, generated_str.find('%'));
		EXPECT_EQ(model.extension(), generated.extension());
		EXPECT_EQ(model.parent_path(), generated.parent_path());
		seen.insert(generated_str);
	}

	EXPECT_EQ(128u, seen.size());
}

TEST(lagi_fs, dir_iterator_bad_directory) {
	std::vector<std::string> files;
	ASSERT_NO_THROW(DirectoryIterator("data/nonexistent", "*.*").GetAll(files));
	EXPECT_TRUE(files.empty());
}

TEST(lagi_fs, dir_iterator_no_filter) {
	std::vector<std::string> files;
	ASSERT_NO_THROW(DirectoryIterator("data/dir_iterator", "").GetAll(files));
	ASSERT_EQ(4u, files.size());
	sort(begin(files), end(files)); // order is not guaranteed
	EXPECT_STREQ("1.a", files[0].c_str());
	EXPECT_STREQ("1.b", files[1].c_str());
	EXPECT_STREQ("2.a", files[2].c_str());
	EXPECT_STREQ("2.b", files[3].c_str());
}

TEST(lagi_fs, dir_iterator_ext_filter) {
	std::vector<std::string> files;
	ASSERT_NO_THROW(DirectoryIterator("data/dir_iterator", "*.a").GetAll(files));
	ASSERT_EQ(2u, files.size());
	sort(begin(files), end(files)); // order is not guaranteed
	EXPECT_STREQ("1.a", files[0].c_str());
	EXPECT_STREQ("2.a", files[1].c_str());
}

TEST(lagi_fs, dir_iterator_ext_filter_skipping_first_works) {
	std::vector<std::string> files;
	ASSERT_NO_THROW(DirectoryIterator("data/dir_iterator", "*.b").GetAll(files));
	ASSERT_EQ(2u, files.size());
	sort(begin(files), end(files)); // order is not guaranteed
	EXPECT_STREQ("1.b", files[0].c_str());
	EXPECT_STREQ("2.b", files[1].c_str());
}

TEST(lagi_fs, dir_iterator_fn_filter) {
	std::vector<std::string> files;
	ASSERT_NO_THROW(DirectoryIterator("data/dir_iterator", "1.*").GetAll(files));
	ASSERT_EQ(2u, files.size());
	sort(begin(files), end(files)); // order is not guaranteed
	EXPECT_STREQ("1.a", files[0].c_str());
	EXPECT_STREQ("1.b", files[1].c_str());
}

TEST(lagi_fs, dir_iterator_all_filtered_out) {
	std::vector<std::string> files;
	ASSERT_NO_THROW(DirectoryIterator("data/dir_iterator", "*.c").GetAll(files));
	EXPECT_TRUE(files.empty());
}
