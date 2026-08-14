#include "external_app_manager.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cJSON.h>
#include <esp_log.h>

namespace agent_ui::external_apps {
namespace {

constexpr char kTag[] = "ExternalApps";
constexpr char kManifestName[] = "manifest.json";
constexpr char kStagingPath[] = "/sdcard/metalio/.installed_apps/.installing";
constexpr char kBackupPath[] = "/sdcard/metalio/.installed_apps/.previous";
constexpr size_t kTarBlockSize = 512;
constexpr size_t kMaxManifestBytes = 8192;
constexpr size_t kMaxPackageEntries = 128;
constexpr size_t kMaxEntryBytes = 8 * 1024 * 1024;
constexpr size_t kMaxExtractedBytes = 16 * 1024 * 1024;

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char link_name[100];
    char magic[6];
    char version[2];
    char owner[32];
    char group[32];
    char device_major[8];
    char device_minor[8];
    char prefix[155];
    char padding[12];
};

static_assert(sizeof(TarHeader) == kTarBlockSize);

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

bool IsDirectory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsRegularFile(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool EnsureDirectoryTree(const std::string& path) {
    if (path.empty() || path[0] != '/') return false;
    for (size_t end = 1; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != '/') continue;
        const std::string part = path.substr(0, end);
        if (part.empty()) continue;
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool RemoveTree(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0;

    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) return false;
    bool okay = true;
    while (dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!RemoveTree(path + "/" + entry->d_name)) okay = false;
    }
    closedir(directory);
    return okay && rmdir(path.c_str()) == 0;
}

bool IsSafeAppId(const std::string& id) {
    if (id.empty() || id.size() > 63 || id.front() == '.') return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' ||
               character == '_' || character == '-';
    });
}

bool IsSafeRelativePath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\') return false;
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos ||
        path.find('\0') != std::string::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") return false;
        if (std::any_of(component.begin(), component.end(), [](unsigned char value) {
                return value < 0x20;
            })) {
            return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool ReadBoundedFile(const std::string& path, size_t limit, std::string* output) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long length = ftell(file);
    if (length < 0 || static_cast<size_t>(length) > limit ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    output->resize(static_cast<size_t>(length));
    const bool okay = output->empty() ||
                      fread(output->data(), 1, output->size(), file) == output->size();
    fclose(file);
    return okay;
}

bool ReadRequiredString(cJSON* root, const char* key, std::string* value) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0') {
        return false;
    }
    *value = item->valuestring;
    return true;
}

bool ParseManifest(const std::string& root_path, AppInfo* app, std::string* error) {
    std::string json;
    if (!ReadBoundedFile(root_path + "/" + kManifestName, kMaxManifestBytes, &json)) {
        SetError(error, "缺少或无法读取 manifest.json");
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) {
        SetError(error, "manifest.json 不是有效 JSON");
        return false;
    }

    bool okay = false;
    do {
        cJSON* manifest_version = cJSON_GetObjectItemCaseSensitive(root, "manifest_version");
        cJSON* api_version = cJSON_GetObjectItemCaseSensitive(root, "api_version");
        std::string target;
        if (!cJSON_IsNumber(manifest_version) || manifest_version->valueint != 1 ||
            !cJSON_IsNumber(api_version) || api_version->valueint != 1 ||
            !ReadRequiredString(root, "id", &app->id) ||
            !ReadRequiredString(root, "name", &app->name) ||
            !ReadRequiredString(root, "version", &app->version) ||
            !ReadRequiredString(root, "target", &target)) {
            SetError(error, "manifest 缺少必填字段或版本不受支持");
            break;
        }
        if (!IsSafeAppId(app->id)) {
            SetError(error, "manifest id 不合法");
            break;
        }
        if (target != "esp32p4") {
            SetError(error, "App 目标芯片不是 esp32p4");
            break;
        }

        std::string entry;
        if (!ReadRequiredString(root, "entry", &entry) || !IsSafeRelativePath(entry)) {
            SetError(error, "manifest entry 不合法");
            break;
        }
        app->root_path = root_path;
        app->entry_path = root_path + "/" + entry;
        if (!IsRegularFile(app->entry_path)) {
            SetError(error, "找不到 ELF 入口文件");
            break;
        }

        cJSON* icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
        if (cJSON_IsString(icon) && icon->valuestring != nullptr && icon->valuestring[0] != '\0') {
            const std::string relative = icon->valuestring;
            if (!IsSafeRelativePath(relative)) {
                SetError(error, "manifest icon 路径不合法");
                break;
            }
            app->icon_path = root_path + "/" + relative;
            if (!IsRegularFile(app->icon_path)) app->icon_path.clear();
        }
        okay = true;
    } while (false);

    cJSON_Delete(root);
    return okay;
}

bool ParseOctal(const char* text, size_t length, size_t* value) {
    size_t result = 0;
    bool saw_digit = false;
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (character == '\0' || character == ' ') {
            if (saw_digit) break;
            continue;
        }
        if (character < '0' || character > '7') return false;
        const size_t digit = character - '0';
        if (result > (SIZE_MAX - digit) / 8U) return false;
        result = result * 8U + digit;
        saw_digit = true;
    }
    if (!saw_digit) return false;
    *value = result;
    return true;
}

bool IsZeroBlock(const unsigned char* block) {
    for (size_t index = 0; index < kTarBlockSize; ++index) {
        if (block[index] != 0) return false;
    }
    return true;
}

bool HasValidChecksum(const unsigned char* block, const TarHeader& header) {
    size_t expected = 0;
    if (!ParseOctal(header.checksum, sizeof(header.checksum), &expected)) return false;
    size_t actual = 0;
    for (size_t index = 0; index < kTarBlockSize; ++index) {
        actual += index >= 148 && index < 156 ? static_cast<unsigned char>(' ')
                                             : block[index];
    }
    return actual == expected;
}

std::string TarText(const char* value, size_t length) {
    const void* terminator = memchr(value, '\0', length);
    const size_t size = terminator == nullptr
                            ? length
                            : static_cast<const char*>(terminator) - value;
    return std::string(value, size);
}

bool SkipBytes(FILE* file, size_t count) {
    unsigned char scratch[kTarBlockSize];
    while (count > 0) {
        const size_t chunk = std::min(count, sizeof(scratch));
        if (fread(scratch, 1, chunk, file) != chunk) return false;
        count -= chunk;
    }
    return true;
}

bool ExtractPackage(const std::string& package_path, std::string* error) {
    FILE* archive = fopen(package_path.c_str(), "rb");
    if (archive == nullptr) {
        SetError(error, "无法打开 App 包");
        return false;
    }

    bool okay = false;
    size_t entry_count = 0;
    size_t extracted_bytes = 0;
    unsigned char block[kTarBlockSize];
    while (fread(block, 1, sizeof(block), archive) == sizeof(block)) {
        if (IsZeroBlock(block)) {
            okay = true;
            break;
        }
        TarHeader header{};
        memcpy(&header, block, sizeof(header));
        if (memcmp(header.magic, "ustar", 5) != 0 || !HasValidChecksum(block, header)) {
            SetError(error, "App 包不是有效的 USTAR 文件");
            break;
        }
        if (++entry_count > kMaxPackageEntries) {
            SetError(error, "App 包文件数量超过限制");
            break;
        }

        std::string relative = TarText(header.name, sizeof(header.name));
        const std::string prefix = TarText(header.prefix, sizeof(header.prefix));
        if (!prefix.empty()) relative = prefix + "/" + relative;
        while (!relative.empty() && relative.back() == '/') relative.pop_back();
        if (!IsSafeRelativePath(relative)) {
            SetError(error, "App 包包含不安全路径");
            break;
        }

        size_t file_size = 0;
        if (!ParseOctal(header.size, sizeof(header.size), &file_size) ||
            file_size > kMaxEntryBytes ||
            extracted_bytes > kMaxExtractedBytes - file_size) {
            SetError(error, "App 包文件大小超过限制");
            break;
        }

        const std::string destination = std::string(kStagingPath) + "/" + relative;
        if (header.type == '5') {
            if (file_size != 0 || !EnsureDirectoryTree(destination)) {
                SetError(error, "无法创建 App 目录");
                break;
            }
        } else if (header.type == '\0' || header.type == '0') {
            const size_t slash = destination.find_last_of('/');
            if (slash == std::string::npos ||
                !EnsureDirectoryTree(destination.substr(0, slash))) {
                SetError(error, "无法创建 App 资源目录");
                break;
            }
            FILE* output = fopen(destination.c_str(), "wb");
            if (output == nullptr) {
                SetError(error, "无法写入 App 文件");
                break;
            }
            size_t remaining = file_size;
            bool write_okay = true;
            while (remaining > 0) {
                const size_t chunk = std::min(remaining, sizeof(block));
                if (fread(block, 1, chunk, archive) != chunk ||
                    fwrite(block, 1, chunk, output) != chunk) {
                    write_okay = false;
                    break;
                }
                remaining -= chunk;
            }
            fclose(output);
            if (!write_okay) {
                SetError(error, "App 包读取或写入失败");
                break;
            }
            const size_t padding =
                (kTarBlockSize - file_size % kTarBlockSize) % kTarBlockSize;
            if (!SkipBytes(archive, padding)) {
                SetError(error, "App 包内容被截断");
                break;
            }
            extracted_bytes += file_size;
        } else {
            SetError(error, "App 包包含不支持的链接或条目类型");
            break;
        }
    }
    fclose(archive);
    return okay;
}

bool HasEappExtension(const std::string& name) {
    if (name.size() < 5) return false;
    std::string extension = name.substr(name.size() - 5);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return std::tolower(value); });
    return extension == ".eapp";
}

bool ActivatePackage(const std::string& package_path, std::string* error) {
    if (!RemoveTree(kStagingPath) || !EnsureDirectoryTree(kStagingPath)) {
        SetError(error, "无法准备 App 临时目录");
        return false;
    }
    if (!ExtractPackage(package_path, error)) {
        RemoveTree(kStagingPath);
        return false;
    }

    AppInfo candidate;
    if (!ParseManifest(kStagingPath, &candidate, error)) {
        RemoveTree(kStagingPath);
        return false;
    }
    const std::string final_path = std::string(Manager::kInstalledRoot) + "/" + candidate.id;
    AppInfo installed;
    if (ParseManifest(final_path, &installed, nullptr) &&
        installed.version == candidate.version) {
        RemoveTree(kStagingPath);
        return true;
    }

    if (!RemoveTree(kBackupPath)) {
        RemoveTree(kStagingPath);
        SetError(error, "无法清理 App 更新备份");
        return false;
    }
    const bool had_previous = IsDirectory(final_path);
    if (had_previous && rename(final_path.c_str(), kBackupPath) != 0) {
        RemoveTree(kStagingPath);
        SetError(error, "无法备份旧版 App");
        return false;
    }
    if (rename(kStagingPath, final_path.c_str()) != 0) {
        if (had_previous) rename(kBackupPath, final_path.c_str());
        RemoveTree(kStagingPath);
        SetError(error, "无法启用 App");
        return false;
    }
    RemoveTree(kBackupPath);
    ESP_LOGI(kTag, "Activated %s %s from %s", candidate.id.c_str(),
             candidate.version.c_str(), package_path.c_str());
    return true;
}

}  // namespace

Manager& Manager::Get() {
    static Manager instance;
    return instance;
}

bool Manager::Refresh(std::string* error) {
    apps_.clear();
    if (!EnsureDirectoryTree(kPackagesRoot) || !EnsureDirectoryTree(kInstalledRoot)) {
        SetError(error, "SD 卡不可用，无法准备 App 目录");
        return false;
    }

    std::vector<std::string> packages;
    if (DIR* package_directory = opendir(kPackagesRoot)) {
        while (dirent* entry = readdir(package_directory)) {
            if (entry->d_name[0] == '.' || !HasEappExtension(entry->d_name)) continue;
            const std::string path = std::string(kPackagesRoot) + "/" + entry->d_name;
            if (IsRegularFile(path)) packages.push_back(path);
        }
        closedir(package_directory);
    }
    std::sort(packages.begin(), packages.end());
    for (const std::string& package : packages) {
        std::string install_error;
        if (!ActivatePackage(package, &install_error)) {
            ESP_LOGW(kTag, "Ignoring %s: %s", package.c_str(), install_error.c_str());
        } else if (unlink(package.c_str()) != 0) {
            ESP_LOGW(kTag, "Installed but could not remove %s: errno=%d",
                     package.c_str(), errno);
        } else {
            ESP_LOGI(kTag, "Removed installed package %s", package.c_str());
        }
    }

    DIR* installed_directory = opendir(kInstalledRoot);
    if (installed_directory == nullptr) {
        SetError(error, "无法读取已展开的 App 目录");
        return false;
    }
    while (dirent* entry = readdir(installed_directory)) {
        if (entry->d_name[0] == '.') continue;
        const std::string root = std::string(kInstalledRoot) + "/" + entry->d_name;
        if (!IsDirectory(root)) continue;
        AppInfo app;
        std::string manifest_error;
        if (ParseManifest(root, &app, &manifest_error)) {
            apps_.push_back(std::move(app));
        } else {
            ESP_LOGW(kTag, "Ignoring %s: %s", root.c_str(), manifest_error.c_str());
        }
    }
    closedir(installed_directory);

    std::sort(apps_.begin(), apps_.end(), [](const AppInfo& left, const AppInfo& right) {
        return left.name < right.name;
    });
    ESP_LOGI(kTag, "Discovered %u installed app(s)",
             static_cast<unsigned>(apps_.size()));
    return true;
}

bool Manager::Select(const std::string& id) {
    const auto found = std::find_if(apps_.begin(), apps_.end(),
                                    [&id](const AppInfo& app) { return app.id == id; });
    if (found == apps_.end()) return false;
    selected_id_ = id;
    return true;
}

const AppInfo* Manager::selected_app() const {
    const auto found = std::find_if(apps_.begin(), apps_.end(),
                                    [this](const AppInfo& app) {
                                        return app.id == selected_id_;
                                    });
    return found == apps_.end() ? nullptr : &*found;
}

}  // namespace agent_ui::external_apps
