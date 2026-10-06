#include "files_directory_repository.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ff.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent_ui::files_directory_repository {
namespace {

enum class ReadOnceResult {
    Success,
    Unavailable,
    Cancelled,
};

bool IsCancelled(const DirectoryReadOptions& options) {
    return options.is_cancelled != nullptr &&
           options.is_cancelled(options.cancel_context);
}

template <typename Visitor>
ReadOnceResult VisitDirectory(const char* directory,
                              const DirectoryReadOptions& options,
                              Visitor visitor) {
    if (IsCancelled(options)) return ReadOnceResult::Cancelled;
    if (directory == nullptr || directory[0] == '\0') {
        return ReadOnceResult::Unavailable;
    }
#ifdef _WIN32
    char search_path[kMaxPathLength + 4] = {};
    const std::size_t path_length = std::strlen(directory);
    if (path_length + 3 >= sizeof(search_path)) return ReadOnceResult::Unavailable;
    std::memcpy(search_path, directory, path_length);
    if (path_length > 0 && directory[path_length - 1] != '/' &&
        directory[path_length - 1] != '\\') {
        search_path[path_length] = '\\';
        search_path[path_length + 1] = '*';
        search_path[path_length + 2] = '\0';
    } else {
        search_path[path_length] = '*';
        search_path[path_length + 1] = '\0';
    }
    WIN32_FIND_DATAA data{};
    HANDLE handle = FindFirstFileA(search_path, &data);
    if (handle == INVALID_HANDLE_VALUE) return ReadOnceResult::Unavailable;
    ReadOnceResult result = ReadOnceResult::Success;
    do {
        if (IsCancelled(options)) { result = ReadOnceResult::Cancelled; break; }
        if (std::strcmp(data.cFileName, ".") == 0 ||
            std::strcmp(data.cFileName, "..") == 0) continue;
        char full_path[kMaxPathLength] = {};
        const bool path_usable =
            JoinPath(full_path, sizeof(full_path), directory, data.cFileName);
        const bool is_directory =
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const uint64_t size =
            (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
            static_cast<uint64_t>(data.nFileSizeLow);
        if (!visitor(data.cFileName, size, is_directory, path_usable)) break;
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    return result;
#else
    DIR* handle = opendir(directory);
    if (handle == nullptr) return ReadOnceResult::Unavailable;
    ReadOnceResult result = ReadOnceResult::Success;
    struct dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        if (IsCancelled(options)) { result = ReadOnceResult::Cancelled; break; }
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) continue;
        char full_path[kMaxPathLength] = {};
        const bool path_usable =
            JoinPath(full_path, sizeof(full_path), directory, entry->d_name);
        struct stat info{};
        const bool stat_succeeded = path_usable && stat(full_path, &info) == 0;
        const bool is_directory = stat_succeeded && S_ISDIR(info.st_mode);
        const uint64_t size = stat_succeeded && info.st_size > 0
                                  ? static_cast<uint64_t>(info.st_size) : 0;
        if (!visitor(entry->d_name, size, is_directory, path_usable)) break;
    }
    closedir(handle);
    return result;
#endif
}

ReadOnceResult ReadDirectoryOnce(const char* directory,
                                 std::vector<Entry>& entries,
                                 const DirectoryReadOptions& options) {
    entries.clear();
    std::size_t entry_limit = options.max_entries == 0
        ? static_cast<std::size_t>(-1) : options.max_entries;
    if (options.max_storage_bytes != 0) {
        const std::size_t storage_limit =
            options.max_storage_bytes / (sizeof(Entry) + 25);
        entry_limit = std::min(entry_limit, storage_limit);
    }
    const bool limited = entry_limit != static_cast<std::size_t>(-1);
    if (limited && entry_limit != 0) entries.reserve(entry_limit);
    std::size_t retained_bytes = 0;
    std::size_t directory_count = 0;
    bool stopped_for_limit = false;
    const std::size_t page_end = !limited
        ? static_cast<std::size_t>(-1)
        : options.offset + entry_limit;
    if (options.truncated != nullptr) *options.truncated = false;
    if (options.had_unaddressable_entries != nullptr)
        *options.had_unaddressable_entries = false;

    const auto append = [&](const char* name, uint64_t size, bool is_directory,
                            bool path_usable, std::size_t global_index) {
        if (global_index < options.offset) return true;
        if (global_index >= page_end) {
            if (options.truncated != nullptr) *options.truncated = true;
            stopped_for_limit = true;
            return false;
        }
        Entry item;
        const std::size_t name_length = std::strlen(name);
        if (options.max_name_bytes != 0 && name_length > options.max_name_bytes) {
            item.name = "[name too long]";
            item.path_usable = false;
            item.name_truncated = true;
        } else {
            item.name.assign(name, name_length);
            item.path_usable = path_usable;
        }
        item.size = size;
        item.is_directory = is_directory;
        if (!item.path_usable && options.had_unaddressable_entries != nullptr)
            *options.had_unaddressable_entries = true;
        const std::size_t cost = sizeof(Entry) + item.name.size() + 1 + 24;
        if (limited && entries.size() >= entry_limit) {
            if (options.truncated != nullptr) *options.truncated = true;
            stopped_for_limit = true;
            return false;
        }
        if (options.max_storage_bytes != 0 &&
            cost > options.max_storage_bytes -
                       std::min(retained_bytes, options.max_storage_bytes)) {
            if (options.truncated != nullptr) *options.truncated = true;
            stopped_for_limit = true;
            return false;
        }
        retained_bytes += cost;
        entries.push_back(std::move(item));
        return true;
    };

    // Two bounded passes provide directory-first pages while preserving the
    // filesystem order inside each group without retaining the full listing.
    const ReadOnceResult dirs_result = VisitDirectory(
        directory, options, [&](const char* name, uint64_t size, bool is_directory,
                                bool path_usable) {
            if (!is_directory) return true;
            return append(name, size, true, path_usable, directory_count++);
        });
    if (dirs_result != ReadOnceResult::Success) {
        entries.clear();
        return dirs_result;
    }
    if (stopped_for_limit) return ReadOnceResult::Success;

    std::size_t file_count = 0;
    const ReadOnceResult files_result = VisitDirectory(
        directory, options, [&](const char* name, uint64_t size, bool is_directory,
                                 bool path_usable) {
            if (is_directory) return true;
            const std::size_t index = directory_count + file_count++;
            return append(name, size, false, path_usable, index);
        });
    if (files_result != ReadOnceResult::Success) {
        entries.clear();
        return files_result;
    }
    return ReadOnceResult::Success;
}

}  // namespace

bool JoinPath(char* output, std::size_t output_size, const char* directory,
              const char* name) {
    if (output == nullptr || output_size == 0 || directory == nullptr ||
        name == nullptr) {
        return false;
    }
    const std::size_t directory_length = std::strlen(directory);
    const std::size_t name_length = std::strlen(name);
    if (directory_length + 1 + name_length + 1 > output_size) return false;
    std::memcpy(output, directory, directory_length);
    output[directory_length] = '/';
    std::memcpy(output + directory_length + 1, name, name_length + 1);
    return true;
}

DirectoryReadResult ReadDirectory(const char* directory, const char* root,
                                  std::vector<Entry>& entries,
                                  const DirectoryReadOptions& options) {
    if (options.truncated != nullptr) *options.truncated = false;
    const ReadOnceResult current_result =
        ReadDirectoryOnce(directory, entries, options);
    if (current_result == ReadOnceResult::Success) {
        return DirectoryReadResult::CurrentDirectory;
    }
    if (current_result == ReadOnceResult::Cancelled) {
        entries.clear();
        return DirectoryReadResult::Cancelled;
    }
    if (root != nullptr && directory != nullptr &&
        std::strcmp(directory, root) != 0) {
        const ReadOnceResult root_result =
            ReadDirectoryOnce(root, entries, options);
        if (root_result == ReadOnceResult::Cancelled) {
            entries.clear();
            return DirectoryReadResult::Cancelled;
        }
        if (root_result == ReadOnceResult::Success) {
            return DirectoryReadResult::FellBackToRoot;
        }
    }
    entries.clear();
    return DirectoryReadResult::Unavailable;
}

bool IsDirectory(const char* path) {
    if (path == nullptr || path[0] == '\0') return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info{};
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

bool RemoveFile(const char* path) {
    if (path == nullptr || path[0] == '\0') return false;
#ifdef _WIN32
    return ::DeleteFileA(path) != 0;
#else
    return unlink(path) == 0;
#endif
}

TextPreviewReadResult ReadTextPreview(const char* path, std::size_t max_bytes,
                                      TextPreview* preview,
                                      bool (*is_cancelled)(void* context),
                                      void* cancel_context) {
    if (path == nullptr || preview == nullptr) {
        return TextPreviewReadResult::OpenFailed;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return TextPreviewReadResult::OpenFailed;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return TextPreviewReadResult::SeekFailed;
    }
    const long file_size = std::ftell(file);
    if (file_size < 0) {
        std::fclose(file);
        return TextPreviewReadResult::SizeUnavailable;
    }
    std::rewind(file);
    preview->truncated = static_cast<uint64_t>(file_size) > max_bytes;
    const std::size_t read_length =
        preview->truncated ? max_bytes : static_cast<std::size_t>(file_size);
    preview->content.clear();
    preview->content.reserve(read_length + 128);
    preview->content.resize(read_length);
    std::size_t bytes_read = 0;
    while (bytes_read < read_length) {
        if (is_cancelled != nullptr && is_cancelled(cancel_context)) {
            std::fclose(file);
            preview->content.clear();
            return TextPreviewReadResult::Cancelled;
        }
        const std::size_t chunk = std::min<std::size_t>(4096,
                                                        read_length - bytes_read);
        const std::size_t count = std::fread(preview->content.data() + bytes_read,
                                             1, chunk, file);
        bytes_read += count;
        if (count < chunk) break;
    }
    if (is_cancelled != nullptr && is_cancelled(cancel_context)) {
        std::fclose(file);
        preview->content.clear();
        return TextPreviewReadResult::Cancelled;
    }
    std::fclose(file);
    preview->content.resize(bytes_read);
    std::replace(preview->content.begin(), preview->content.end(), '\0', ' ');
    return TextPreviewReadResult::Success;
}

bool ReadStorageBytes(uint64_t* total_bytes, uint64_t* free_bytes) {
    if (total_bytes == nullptr || free_bytes == nullptr) return false;
    *total_bytes = 0;
    *free_bytes = 0;
    FATFS* filesystem = nullptr;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &filesystem) != FR_OK ||
        filesystem == nullptr) {
        return false;
    }
    constexpr DWORD kBytesPerSector = 512;
    *total_bytes = static_cast<uint64_t>(filesystem->n_fatent - 2) *
                   filesystem->csize * kBytesPerSector;
    *free_bytes = static_cast<uint64_t>(free_clusters) * filesystem->csize *
                  kBytesPerSector;
    return true;
}

}  // namespace agent_ui::files_directory_repository
