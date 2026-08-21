#include "camera_gallery_repository.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "SdCardManager.hpp"
#include "esp_timer.h"

namespace agent_ui::camera {
namespace {

constexpr char kDcimDirectory[] = "/sdcard/DCIM";
constexpr char kCameraDirectory[] = "/sdcard/DCIM/Camera";
constexpr std::size_t kMaxItems = 48;

bool IsJpg(const char* name) {
    if (name == nullptr) return false;
    const std::size_t length = std::strlen(name);
    if (length < 5) return false;
    const char* extension = name + length - 4;
    const bool jpg = extension[0] == '.' &&
                     (extension[1] == 'j' || extension[1] == 'J') &&
                     (extension[2] == 'p' || extension[2] == 'P') &&
                     (extension[3] == 'g' || extension[3] == 'G');
    if (jpg) return true;
    if (length < 6) return false;
    extension = name + length - 5;
    return extension[0] == '.' &&
           (extension[1] == 'j' || extension[1] == 'J') &&
           (extension[2] == 'p' || extension[2] == 'P') &&
           (extension[3] == 'e' || extension[3] == 'E') &&
           (extension[4] == 'g' || extension[4] == 'G');
}

bool IsCameraPath(const std::string& path) {
    const std::string prefix = std::string(kCameraDirectory) + "/";
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
           path.find('/', prefix.size()) == std::string::npos;
}

bool EnsureDirectory(const char* path) {
    struct stat info = {};
    if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
    if (mkdir(path, 0775) == 0) return true;
    if (errno != EEXIST || stat(path, &info) != 0) return false;
    return S_ISDIR(info.st_mode);
}

bool EnsureCameraDirectory() {
    if (!EnsureDirectory(kDcimDirectory) || !EnsureDirectory(kCameraDirectory)) {
        return false;
    }
    return true;
}

std::string FormatDate(const std::tm& local, char separator) {
    const long long raw_year = static_cast<long long>(local.tm_year) + 1900;
    const unsigned year = static_cast<unsigned>(
        std::clamp(raw_year, 0LL, 9999LL));
    const unsigned month = static_cast<unsigned>(std::clamp(
        static_cast<long long>(local.tm_mon) + 1, 1LL, 12LL));
    const unsigned day = static_cast<unsigned>(std::clamp(
        static_cast<long long>(local.tm_mday), 1LL, 31LL));

    std::string result(10, '0');
    result[0] = static_cast<char>('0' + year / 1000);
    result[1] = static_cast<char>('0' + (year / 100) % 10);
    result[2] = static_cast<char>('0' + (year / 10) % 10);
    result[3] = static_cast<char>('0' + year % 10);
    result[4] = separator;
    result[5] = static_cast<char>('0' + month / 10);
    result[6] = static_cast<char>('0' + month % 10);
    result[7] = separator;
    result[8] = static_cast<char>('0' + day / 10);
    result[9] = static_cast<char>('0' + day % 10);
    return result;
}

std::string FormatDayLabel(const std::tm& local) {
    char value[32];
    std::snprintf(value, sizeof(value), "%d月%d日", local.tm_mon + 1,
                  local.tm_mday);
    return value;
}

std::string FormatTimeLabel(const std::tm& local) {
    char value[8];
    std::snprintf(value, sizeof(value), "%02d:%02d", local.tm_hour,
                  local.tm_min);
    return value;
}

std::string FormatSizeLabel(std::size_t bytes) {
    if (bytes == 0) return "大小未知";
    char value[24];
    if (bytes < 1024 * 1024) {
        std::snprintf(value, sizeof(value), "%u KB",
                      static_cast<unsigned>((bytes + 1023) / 1024));
    } else {
        std::snprintf(value, sizeof(value), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return value;
}

}  // namespace

std::vector<GalleryPhoto> GalleryRepository::List() const {
    std::vector<GalleryPhoto> result;
    if (!SdCardManager::GetInstance().IsMounted() || !EnsureCameraDirectory()) {
        return result;
    }

    DIR* directory = opendir(kCameraDirectory);
    if (directory == nullptr) return result;
    while (struct dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        if (entry->d_type == DT_DIR ||
            (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) ||
            !IsJpg(entry->d_name)) {
            continue;
        }
        GalleryPhoto photo;
        photo.name = entry->d_name;
        photo.path = std::string(kCameraDirectory) + "/" + entry->d_name;
        struct stat info = {};
        if (stat(photo.path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        photo.modified = info.st_mtime;
        photo.bytes = static_cast<std::size_t>(info.st_size);
        std::tm local = {};
        localtime_r(&info.st_mtime, &local);
        photo.date_key = FormatDate(local, '-');
        photo.date_label = FormatDayLabel(local);
        photo.time_label = FormatTimeLabel(local);
        photo.size_label = FormatSizeLabel(photo.bytes);
        result.emplace_back(std::move(photo));
    }
    closedir(directory);

    std::sort(result.begin(), result.end(), [](const GalleryPhoto& left,
                                               const GalleryPhoto& right) {
        if (left.modified != right.modified) return left.modified > right.modified;
        return left.name > right.name;
    });
    if (result.size() > kMaxItems) result.resize(kMaxItems);
    return result;
}

bool GalleryRepository::Delete(const std::string& path) const {
    return IsCameraPath(path) && unlink(path.c_str()) == 0;
}

bool GalleryRepository::WriteJpeg(const std::vector<uint8_t>& jpeg,
                                  std::string* path) const {
    if (jpeg.empty() || !SdCardManager::GetInstance().IsMounted() ||
        !EnsureCameraDirectory()) {
        return false;
    }
    char output_path[96];
    const unsigned long timestamp =
        static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
    FILE* file = nullptr;
    for (unsigned attempt = 0; attempt < 1000 && file == nullptr; ++attempt) {
        std::snprintf(output_path, sizeof(output_path), "%s/IMG_%lu.jpg",
                      kCameraDirectory, timestamp + attempt);
        if (access(output_path, F_OK) != 0) file = fopen(output_path, "wb");
    }
    if (file == nullptr) {
        return false;
    }
    const std::size_t written = fwrite(jpeg.data(), 1, jpeg.size(), file);
    fclose(file);
    if (written != jpeg.size()) {
        unlink(output_path);
        return false;
    }
    if (path != nullptr) *path = output_path;
    return true;
}

}  // namespace agent_ui::camera
