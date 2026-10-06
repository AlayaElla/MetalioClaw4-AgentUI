#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agent_ui::files_directory_repository {

inline constexpr std::size_t kMaxPathLength = 512;

struct Entry {
    std::string name;
    uint64_t size = 0;
    bool is_directory = false;
    bool path_usable = true;
    bool name_truncated = false;
};

enum class DirectoryReadResult {
    CurrentDirectory,
    FellBackToRoot,
    Unavailable,
    Cancelled,
};

struct DirectoryReadOptions {
    // Zero preserves the unbounded repository API. UI jobs set a nonzero limit
    // so a damaged or unusually large card cannot consume unbounded RAM.
    std::size_t max_entries = 0;
    std::size_t offset = 0;
    // Zero preserves all names; UI pages choose 255, matching the SD/FAT limit.
    std::size_t max_name_bytes = 0;
    // Includes Entry objects, copied names and a conservative per-string
    // allocator allowance. UI pages stay within this retained-memory budget.
    std::size_t max_storage_bytes = 0;
    bool (*is_cancelled)(void* context) = nullptr;
    void* cancel_context = nullptr;
    bool* truncated = nullptr;
    bool* had_unaddressable_entries = nullptr;
};

struct TextPreview {
    std::string content;
    bool truncated = false;
};

enum class TextPreviewReadResult {
    Success,
    OpenFailed,
    SeekFailed,
    SizeUnavailable,
    Cancelled,
};

bool JoinPath(char* output, std::size_t output_size, const char* directory,
              const char* name);
DirectoryReadResult ReadDirectory(const char* directory, const char* root,
                                  std::vector<Entry>& entries,
                                  const DirectoryReadOptions& options = {});
bool IsDirectory(const char* path);
bool RemoveFile(const char* path);
TextPreviewReadResult ReadTextPreview(const char* path, std::size_t max_bytes,
                                      TextPreview* preview,
                                      bool (*is_cancelled)(void* context) = nullptr,
                                      void* cancel_context = nullptr);
bool ReadStorageBytes(uint64_t* total_bytes, uint64_t* free_bytes);

}  // namespace agent_ui::files_directory_repository
