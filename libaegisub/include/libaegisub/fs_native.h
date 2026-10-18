#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <ctime>
#include <system_error>

namespace agi::fs::detail {

enum class FileEntryType {
	missing,
	regular,
	directory,
	other
};

struct FileInfo {
	FileEntryType type = FileEntryType::missing;
	uintmax_t size = 0;
	time_t modified_time = 0;
};

#ifdef _WIN32
path NativePath(path const& path, std::error_code& ec);
#endif

bool TryGetFileInfo(path const& path, FileInfo& out, std::error_code& ec);
void Rename(path const& from, path const& to, std::error_code& ec);
}
