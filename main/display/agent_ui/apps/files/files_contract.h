#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "apps/files/files_directory_repository.h"

namespace agent_ui::files {

inline constexpr std::size_t kMaxPathLength =
    files_directory_repository::kMaxPathLength;

enum class Availability : uint8_t { Ready, Missing, UsbBusy, UsbExported };
enum class ItemKind : uint8_t { File, PreviousPage, NextPage, Notice };
enum class PreviewKind : uint8_t { None, TextLoading, Text, ImagePending, Image };
enum class Status : uint8_t { Unknown, Mounted, Missing, UsbBusy, UsbExported, ReadError };

struct DirectoryItem {
    files_directory_repository::Entry entry;
    ItemKind kind = ItemKind::File;
};

// Immutable-to-the-view state. It contains presentation data, never LVGL
// objects, USB leases, or board/service pointers.
struct ViewState {
    std::string root;
    std::string directory;
    std::vector<DirectoryItem> entries;
    uint32_t directory_generation = 0;
    uint32_t preview_generation = 0;
    uint32_t row_epoch = 0;
    std::size_t page_offset = 0;
    std::size_t next_page_offset = 0;
    std::size_t real_entry_count = 0;
    bool directory_loading = false;
    bool page_truncated = false;
    bool has_unaddressable_entries = false;
    bool view_active = false;
    Status status = Status::Unknown;

    PreviewKind preview_kind = PreviewKind::None;
    std::string preview_path;
    std::string preview_text;
    bool preview_truncated = false;
    bool preview_failed = false;

    uint64_t capacity_total = 0;
    uint64_t capacity_free = 0;
    bool capacity_available = false;
    bool capacity_loading = false;
};

struct UsbState {
    bool supported = false;
    bool active = false;
    bool busy = false;
};

}  // namespace agent_ui::files
