#include "external_app_symbols.h"

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_elf.h"
#include "esp_log.h"

extern "C" {
double __adddf3(double left, double right);
double __divdf3(double left, double right);
int __gedf2(double left, double right);
int __gtdf2(double left, double right);
int __ltdf2(double left, double right);
double __muldf3(double left, double right);
int __nedf2(double left, double right);
double __subdf3(double left, double right);
int __unorddf2(double left, double right);
}

namespace agent_ui::external_apps {
namespace {

constexpr char kTag[] = "ExternalAppSymbols";

double HostLog(double value) { return std::log(value); }
double HostSqrt(double value) { return std::sqrt(value); }
double HostRound(double value) { return std::round(value); }
double HostFmod(double left, double right) { return std::fmod(left, right); }
double HostCos(double value) { return std::cos(value); }
double HostSin(double value) { return std::sin(value); }
double HostPow(double base, double exponent) { return std::pow(base, exponent); }
double HostLog10(double value) { return std::log10(value); }
double HostTan(double value) { return std::tan(value); }

int HostSnprintf(char* output, size_t capacity, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(output, capacity, format, arguments);
    va_end(arguments);
    return written;
}

void* HostMemcpy(void* destination, const void* source, size_t count) {
    return std::memcpy(destination, source, count);
}

void* HostMemset(void* destination, int value, size_t count) {
    return std::memset(destination, value, count);
}

void* HostMemchr(const void* data, int value, size_t count) {
    return const_cast<void*>(std::memchr(data, value, count));
}

int HostMemcmp(const void* left, const void* right, size_t count) {
    return std::memcmp(left, right, count);
}

int HostStrcmp(const char* left, const char* right) {
    return std::strcmp(left, right);
}

int HostStrncmp(const char* left, const char* right, size_t count) {
    return std::strncmp(left, right, count);
}

size_t HostStrlen(const char* text) { return std::strlen(text); }

double HostStrtod(const char* text, char** end) {
    return std::strtod(text, end);
}

const struct esp_elfsym kExternalAppSymbols[] = {
#define METALIO_APP_IMPORT(imported_name, host_target) \
    {#imported_name, reinterpret_cast<const void*>(&host_target)},
#include "metalio_app_host_imports.def"
#undef METALIO_APP_IMPORT
    ESP_ELFSYM_END,
};

}  // namespace

bool EnsureExternalAppSymbolsRegistered() {
    static bool registered = false;
    if (registered) return true;

    const int result = esp_elf_register_symbol(kExternalAppSymbols);
    if (result == 0 || result == -EEXIST) {
        registered = true;
        return true;
    }
    ESP_LOGE(kTag, "Failed to register external App symbols: %d", result);
    return false;
}

}  // namespace agent_ui::external_apps
