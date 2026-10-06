#include "usb_virtual_disk.h"

// Keep the real lease implementation in one target-independent translation
// unit. Both the ESP32-P4 MSC path and the non-P4 stub provide their own
// TryBegin/End methods, while sharing this RAII lifetime behavior.
UsbVirtualDisk::SdLocalAccess::SdLocalAccess(UsbVirtualDisk& disk)
    : disk_(&disk), acquired_(disk.TryBeginSdLocalAccess()) {
    if (!acquired_) disk_ = nullptr;
}

UsbVirtualDisk::SdLocalAccess::~SdLocalAccess() {
    if (acquired_ && disk_ != nullptr) disk_->EndSdLocalAccess();
}
