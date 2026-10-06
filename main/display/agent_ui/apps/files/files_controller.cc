#include "apps/files/files_controller.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace agent_ui::files {
namespace {

constexpr std::size_t kDirectoryPageEntries = 48;

bool ExtensionEquals(const char* path, const char* expected) {
    if (path == nullptr || expected == nullptr) return false;
    const char* slash = std::strrchr(path, '/');
    const char* dot = std::strrchr(slash != nullptr ? slash + 1 : path, '.');
    if (dot == nullptr) return false;
    ++dot;
    while (*dot != '\0' && *expected != '\0') {
        if (std::tolower(static_cast<unsigned char>(*dot)) !=
            std::tolower(static_cast<unsigned char>(*expected))) return false;
        ++dot;
        ++expected;
    }
    return *dot == '\0' && *expected == '\0';
}

}  // namespace

Controller::Controller(Port& port) : port_(port) {}

void Controller::SetStateChanged(std::function<void(const ViewState&)> callback) {
    state_changed_ = std::move(callback);
}

uint32_t Controller::NextGeneration(uint32_t current) {
    ++current;
    return current == 0 ? 1 : current;
}

void Controller::NotifyStateChanged() {
    auto callback = state_changed_;
    if (callback) callback(state_);
}

void Controller::SetStatusFromAvailability(Availability availability) {
    if (availability_known_ && availability != last_availability_)
        port_.InvalidateCapacity();
    last_availability_ = availability;
    availability_known_ = true;
    switch (availability) {
        case Availability::Ready: state_.status = Status::Mounted; break;
        case Availability::Missing: state_.status = Status::Missing; break;
        case Availability::UsbBusy: state_.status = Status::UsbBusy; break;
        case Availability::UsbExported: state_.status = Status::UsbExported; break;
    }
    if (availability != Availability::Ready) {
        state_.capacity_available = false;
        state_.capacity_total = 0;
        state_.capacity_free = 0;
        state_.capacity_loading = false;
        capacity_ticket_.reset();
    }
}

void Controller::Load() {
    state_.root = port_.GetRootPath();
    if (state_.root.empty()) state_.root = "/sdcard";
    if (state_.directory.empty()) state_.directory = state_.root;
    state_.view_active = true;
    RefreshStatus();
    RefreshDirectory();
    RefreshCapacity();
}

void Controller::Resume() {
    state_.view_active = true;
    if (state_.root.empty()) {
        Load();
        return;
    }
    RefreshStatus();
    RefreshDirectory();
    RefreshCapacity();
}

void Controller::Suspend() {
    state_.view_active = false;
    CancelDirectoryRequest();
    state_.capacity_loading = false;
    state_.capacity_available = false;
    state_.capacity_total = 0;
    state_.capacity_free = 0;
    capacity_ticket_.reset();
    ClosePreviewState();
    NotifyStateChanged();
}

void Controller::Unload() {
    state_.view_active = false;
    CancelDirectoryRequest();
    ClosePreviewState();
    CancelDeleteRequest();
    state_.entries.clear();
    state_.directory.clear();
    state_.root.clear();
    state_.directory_loading = false;
    state_.capacity_loading = false;
    state_.capacity_available = false;
    state_.capacity_total = 0;
    state_.capacity_free = 0;
    capacity_ticket_.reset();
    state_.row_epoch = NextGeneration(state_.row_epoch);
    NotifyStateChanged();
}

void Controller::RefreshStatus() {
    const Availability availability = port_.GetAvailability();
    SetStatusFromAvailability(availability);
    NotifyStateChanged();
}

void Controller::CancelDirectoryRequest() {
    if (directory_ticket_ != nullptr) {
        directory_ticket_->RequestCancellation();
        files_io_worker::DirectoryResult discarded;
        port_.TakeDirectory(directory_ticket_->id(), &discarded);
        directory_ticket_.reset();
    }
}

void Controller::CancelPreviewRequest() {
    if (preview_ticket_ != nullptr) {
        if (preview_dispatched_) {
            preview_ticket_->RequestCancellation();
            files_io_worker::PreviewResult discarded;
            port_.TakePreview(preview_ticket_->id(), &discarded);
        } else {
            port_.CancelPreparedPreview(preview_ticket_);
        }
        preview_ticket_.reset();
    }
    preview_dispatched_ = false;
    if (image_ticket_ != nullptr) {
        if (image_ticket_->phase() == files_io_worker::Ticket::Phase::Started)
            image_ticket_->Finish(false);
        image_ticket_.reset();
    }
    pending_preview_path_.clear();
    pending_preview_generation_ = 0;
}

void Controller::CancelDeleteRequest() {
    if (delete_ticket_ != nullptr) {
        delete_ticket_->CancelBeforeStart();
        // A started delete is allowed to finish and will still be drained by
        // ApplyPendingResults; cancelling it after admission would be untrue.
        if (delete_ticket_->phase() == files_io_worker::Ticket::Phase::Cancelled ||
            delete_ticket_->IsDone()) {
            delete_ticket_.reset();
            delete_path_.clear();
        }
    }
}

void Controller::RefreshDirectory() {
    if (!state_.view_active) return;
    RequestDirectory();
}

void Controller::RequestDirectory() {
    state_.directory_generation = NextGeneration(state_.directory_generation);
    state_.row_epoch = NextGeneration(state_.row_epoch);
    CancelDirectoryRequest();
    state_.entries.clear();
    state_.real_entry_count = 0;
    state_.page_truncated = false;
    state_.has_unaddressable_entries = false;
    state_.directory_loading = false;

    const Availability availability = port_.GetAvailability();
    SetStatusFromAvailability(availability);
    if (availability != Availability::Ready) {
        NotifyStateChanged();
        return;
    }
    if (state_.directory.empty()) state_.directory = state_.root;
    directory_ticket_ = files_io_worker::CreateTicket();
    state_.directory_loading = true;
    if (!port_.SubmitDirectory(directory_ticket_, state_.directory.c_str(),
                               state_.root.c_str(), state_.page_offset,
                               state_.directory_generation)) {
        directory_ticket_.reset();
        state_.directory_loading = false;
        state_.status = Status::ReadError;
    }
    NotifyStateChanged();
}

void Controller::RefreshCapacity() {
    if (!state_.view_active) return;
    const Availability availability = port_.GetAvailability();
    SetStatusFromAvailability(availability);
    if (availability != Availability::Ready) {
        NotifyStateChanged();
        return;
    }
    capacity_ticket_ = port_.RequestCapacity(state_.directory_generation);
    uint64_t total = 0;
    uint64_t free = 0;
    if (capacity_ticket_ != nullptr && capacity_ticket_->GetCapacity(&total, &free)) {
        state_.capacity_total = total;
        state_.capacity_free = free;
        state_.capacity_available = true;
        state_.capacity_loading = false;
    } else if (capacity_ticket_ != nullptr && !capacity_ticket_->IsDone()) {
        state_.capacity_loading = true;
    } else if (port_.GetCapacityFallback(&total, &free)) {
        state_.capacity_total = total;
        state_.capacity_free = free;
        state_.capacity_available = true;
        state_.capacity_loading = false;
    } else {
        state_.capacity_loading = false;
    }
    NotifyStateChanged();
}

void Controller::ApplyDirectoryResult(
    files_io_worker::DirectoryResult&& result,
    const files_io_worker::TicketPtr& ticket) {
    const bool current = state_.view_active &&
        result.generation == state_.directory_generation &&
        result.directory == state_.directory;
    if (!current || ticket == nullptr) return;
    state_.directory_loading = false;
    if (result.status == files_directory_repository::DirectoryReadResult::FellBackToRoot) {
        // A fallback result describes the failed directory only. Issue a fresh
        // root request so the visible entries always correspond to the path.
        state_.directory = state_.root;
        state_.page_offset = 0;
        RequestDirectory();
        return;
    }
    if (result.status != files_directory_repository::DirectoryReadResult::CurrentDirectory) {
        if (ticket->phase() != files_io_worker::Ticket::Phase::Cancelled)
            state_.status = Status::ReadError;
        return;
    }

    state_.entries.clear();
    state_.entries.reserve(result.entries.size() + 3);
    state_.real_entry_count = result.entries.size();
    state_.page_truncated = result.truncated;
    state_.has_unaddressable_entries = result.had_unaddressable_entries;
    state_.next_page_offset = state_.page_offset + state_.real_entry_count;
    if (state_.page_offset > 0) {
        DirectoryItem previous;
        previous.kind = ItemKind::PreviousPage;
        state_.entries.push_back(std::move(previous));
    }
    for (auto& entry : result.entries) {
        DirectoryItem item;
        item.entry = std::move(entry);
        state_.entries.push_back(std::move(item));
    }
    if (state_.has_unaddressable_entries) {
        DirectoryItem notice;
        notice.kind = ItemKind::Notice;
        state_.entries.push_back(std::move(notice));
    }
    if (state_.page_truncated) {
        DirectoryItem next;
        next.kind = ItemKind::NextPage;
        state_.entries.push_back(std::move(next));
    }
    state_.status = Status::Mounted;
}

void Controller::ApplyPreviewResult(
    files_io_worker::PreviewResult&& result,
    const files_io_worker::TicketPtr& ticket) {
    const bool current = state_.view_active &&
        preview_dispatched_ && result.ticket_id == ticket->id() &&
        result.generation == pending_preview_generation_ &&
        result.path == pending_preview_path_;
    if (!current || ticket == nullptr) return;
    preview_ticket_.reset();
    preview_dispatched_ = false;
    pending_preview_path_.clear();
    pending_preview_generation_ = 0;
    if (result.status == files_directory_repository::TextPreviewReadResult::Success) {
        state_.preview_generation = result.generation;
        state_.preview_path = result.path;
        state_.preview_kind = PreviewKind::Text;
        state_.preview_text = std::move(result.preview.content);
        state_.preview_truncated = result.preview.truncated;
        state_.preview_failed = false;
    } else {
        state_.preview_generation = result.generation;
        state_.preview_path = result.path;
        state_.preview_kind = PreviewKind::Text;
        state_.preview_text.clear();
        state_.preview_truncated = false;
        state_.preview_failed = true;
        state_.status = Status::ReadError;
    }
}

void Controller::ApplyPendingResults() {
    bool changed = false;
    if (directory_ticket_ != nullptr) {
        files_io_worker::DirectoryResult result;
        if (port_.TakeDirectory(directory_ticket_->id(), &result)) {
            auto ticket = std::move(directory_ticket_);
            ApplyDirectoryResult(std::move(result), ticket);
            changed = true;
        }
    }
    if (preview_ticket_ != nullptr && preview_dispatched_) {
        files_io_worker::PreviewResult result;
        if (port_.TakePreview(preview_ticket_->id(), &result)) {
            auto ticket = std::move(preview_ticket_);
            ApplyPreviewResult(std::move(result), ticket);
            changed = true;
        }
    }
    if (delete_ticket_ != nullptr) {
        bool succeeded = false;
        if (port_.TakeDelete(delete_ticket_->id(), &succeeded)) {
            const bool current = state_.view_active &&
                                 delete_generation_ == state_.directory_generation;
            delete_ticket_.reset();
            if (current) {
                if (succeeded) {
                    if (delete_path_ == state_.preview_path) ClosePreviewState();
                    RequestDirectory();
                    RefreshCapacity();
                } else {
                    state_.status = Status::ReadError;
                }
            }
            delete_path_.clear();
            changed = true;
        }
    }
    if (capacity_ticket_ != nullptr && capacity_ticket_->IsDone()) {
        RefreshCapacity();
        changed = true;
    }
    if (changed) NotifyStateChanged();
}

bool Controller::NavigateBack() {
    if (state_.directory.empty() || state_.directory == state_.root) return false;
    const std::size_t root_length = state_.root.size();
    const std::size_t slash = state_.directory.find_last_of('/');
    if (slash == std::string::npos || slash <= root_length) {
        state_.directory = state_.root;
    } else {
        state_.directory.resize(slash);
    }
    state_.page_offset = 0;
    RequestDirectory();
    return true;
}

void Controller::PreviousPage() {
    state_.page_offset = state_.page_offset > kDirectoryPageEntries
                             ? state_.page_offset - kDirectoryPageEntries : 0;
    RequestDirectory();
}

void Controller::NextPage() {
    if (!state_.page_truncated || state_.next_page_offset <= state_.page_offset) return;
    state_.page_offset = state_.next_page_offset;
    RequestDirectory();
}

bool Controller::IsValidPath(const char* path) const {
    if (path == nullptr || state_.root.empty()) return false;
    const std::size_t length = std::strlen(path);
    if (length == 0 || length >= kMaxPathLength ||
        std::strncmp(path, state_.root.c_str(), state_.root.size()) != 0 ||
        path[state_.root.size()] != '/' || std::strstr(path, "..") != nullptr ||
        std::strchr(path, '\\') != nullptr) return false;
    return true;
}

bool Controller::IsPreviewablePath(const char* path, bool* image) const {
    const bool is_image = ExtensionEquals(path, "jpg") || ExtensionEquals(path, "jpeg") ||
                          ExtensionEquals(path, "png") || ExtensionEquals(path, "sjpg");
    const bool is_text = ExtensionEquals(path, "txt");
    if (image != nullptr) *image = is_image;
    return is_image || is_text;
}

void Controller::ActivateEntry(std::size_t index, uint32_t row_epoch) {
    if (!state_.view_active || row_epoch != state_.row_epoch ||
        index >= state_.entries.size() || port_.GetAvailability() != Availability::Ready)
        return;
    const DirectoryItem& item = state_.entries[index];
    if (item.kind == ItemKind::PreviousPage) {
        PreviousPage();
        return;
    }
    if (item.kind == ItemKind::NextPage) {
        NextPage();
        return;
    }
    if (item.kind != ItemKind::File || !item.entry.path_usable) return;
    char path[kMaxPathLength] = {};
    if (!files_directory_repository::JoinPath(path, sizeof(path),
                                              state_.directory.c_str(),
                                              item.entry.name.c_str())) return;
    if (item.entry.is_directory) {
        state_.directory = path;
        state_.page_offset = 0;
        RequestDirectory();
        return;
    }
    if (IsPreviewablePath(path, nullptr)) PreviewPath(path);
}

bool Controller::PreviewPath(const char* path,
                             const files_io_worker::TicketPtr& supplied_ticket) {
    bool image = false;
    if (!CanPreviewPath(path, supplied_ticket) ||
        !IsPreviewablePath(path, &image)) return false;
    const uint32_t generation = NextGeneration(next_preview_generation_);
    files_io_worker::TicketPtr candidate;
    if (image) {
        candidate = supplied_ticket != nullptr ? supplied_ticket
                                                : files_io_worker::CreateTicket();
        if (!candidate->Queue() || !candidate->TryStart()) return false;
        CancelPreviewRequest();
        next_preview_generation_ = generation;
        image_ticket_ = std::move(candidate);
        pending_preview_path_ = path;
        pending_preview_generation_ = generation;
    } else {
        candidate = supplied_ticket != nullptr ? supplied_ticket
                                                : files_io_worker::CreateTicket();
        if (!port_.SubmitPreview(candidate, path, generation)) {
            return false;
        }
        // The submission boundary is allowed to race a cancellation. Do not
        // disturb the currently displayed preview unless queue admission won.
        if (candidate->phase() != files_io_worker::Ticket::Phase::Queued) {
            port_.CancelPreparedPreview(candidate);
            return false;
        }
        CancelPreviewRequest();
        next_preview_generation_ = generation;
        preview_ticket_ = std::move(candidate);
        pending_preview_path_ = path;
        pending_preview_generation_ = generation;
        preview_dispatched_ = false;
    }
    NotifyStateChanged();
    return true;
}

bool Controller::CanPreviewPath(
    const char* path, const files_io_worker::TicketPtr& ticket) const {
    return state_.view_active && IsValidPath(path) &&
           IsPreviewablePath(path, nullptr) &&
           port_.GetAvailability() == Availability::Ready &&
           (ticket == nullptr ||
            ticket->phase() == files_io_worker::Ticket::Phase::NotAdmitted);
}

bool Controller::RequestDeletePath(
    const char* path, const files_io_worker::TicketPtr& ticket) {
    if (!state_.view_active || !IsValidPath(path) ||
        port_.GetAvailability() != Availability::Ready ||
        (delete_ticket_ != nullptr && !delete_ticket_->IsDone())) return false;
    const auto candidate = ticket != nullptr ? ticket
                                               : files_io_worker::CreateTicket();
    if (!CanDeletePath(path, candidate)) return false;
    delete_ticket_ = candidate;
    delete_path_ = path;
    delete_generation_ = state_.directory_generation;
    if (!port_.SubmitDelete(candidate, path, delete_generation_)) {
        delete_ticket_.reset();
        delete_path_.clear();
        return false;
    }
    NotifyStateChanged();
    return true;
}

bool Controller::CanDeletePath(
    const char* path, const files_io_worker::TicketPtr& ticket) const {
    return state_.view_active && IsValidPath(path) &&
           port_.GetAvailability() == Availability::Ready &&
           (delete_ticket_ == nullptr || delete_ticket_->IsDone()) &&
           (ticket == nullptr ||
            ticket->phase() == files_io_worker::Ticket::Phase::NotAdmitted);
}

bool Controller::DeletePathSync(const char* path) {
    return state_.view_active && IsValidPath(path) &&
           port_.GetAvailability() == Availability::Ready &&
           port_.DeletePathSync(path);
}

bool Controller::ReadStorageBytes(uint64_t* total, uint64_t* free) {
    return total != nullptr && free != nullptr &&
           port_.GetAvailability() == Availability::Ready &&
           port_.ReadStorageBytes(total, free);
}

bool Controller::IsPreviewFor(const char* path) const {
    if (path == nullptr || state_.preview_path != path) return false;
    return state_.preview_kind == PreviewKind::Text ||
           state_.preview_kind == PreviewKind::Image;
}

bool Controller::HasPendingImagePresentation() const {
    return state_.view_active && image_ticket_ != nullptr &&
           !pending_preview_path_.empty();
}

bool Controller::HasPendingTextPreview() const {
    return state_.view_active && preview_ticket_ != nullptr &&
           !preview_dispatched_ && !pending_preview_path_.empty() &&
           preview_ticket_->phase() == files_io_worker::Ticket::Phase::Queued;
}

bool Controller::StartPendingTextPreview() {
    if (!HasPendingTextPreview() ||
        port_.GetAvailability() != Availability::Ready) return false;
    if (port_.StartPreparedPreview(preview_ticket_)) return true;
    AbortPendingTextPreview();
    return false;
}

bool Controller::DispatchPendingTextPreview() {
    if (preview_ticket_ == nullptr || preview_dispatched_ ||
        pending_preview_path_.empty() ||
        preview_ticket_->phase() != files_io_worker::Ticket::Phase::Started)
        return false;
    if (!port_.DispatchPreparedPreview(preview_ticket_,
                                      pending_preview_path_.c_str(),
                                      pending_preview_generation_)) {
        preview_ticket_->Finish(false);
        preview_ticket_.reset();
        pending_preview_path_.clear();
        pending_preview_generation_ = 0;
        state_.status = Status::ReadError;
        NotifyStateChanged();
        return false;
    }
    preview_dispatched_ = true;
    state_.preview_generation = pending_preview_generation_;
    state_.preview_path = pending_preview_path_;
    state_.preview_kind = PreviewKind::TextLoading;
    state_.preview_text.clear();
    state_.preview_truncated = false;
    state_.preview_failed = false;
    NotifyStateChanged();
    return true;
}

void Controller::AbortPendingTextPreview() {
    if (preview_ticket_ == nullptr || preview_dispatched_) return;
    port_.CancelPreparedPreview(preview_ticket_);
    preview_ticket_.reset();
    pending_preview_path_.clear();
    pending_preview_generation_ = 0;
}

void Controller::DropDisplayedPreviewForReplacement() {
    const bool changed = state_.preview_kind != PreviewKind::None ||
                         !state_.preview_path.empty() ||
                         !state_.preview_text.empty();
    state_.preview_generation = NextGeneration(state_.preview_generation);
    state_.preview_kind = PreviewKind::None;
    state_.preview_path.clear();
    state_.preview_text.clear();
    state_.preview_truncated = false;
    state_.preview_failed = false;
    if (changed) NotifyStateChanged();
}

bool Controller::CompleteImagePresentation(bool presented,
                                           bool previous_preview_cleared) {
    if (image_ticket_ == nullptr || pending_preview_path_.empty())
        return false;
    const bool current = state_.view_active && presented &&
                         port_.GetAvailability() == Availability::Ready;
    if (current) {
        state_.preview_generation = pending_preview_generation_;
        state_.preview_path = pending_preview_path_;
        state_.preview_kind = PreviewKind::Image;
        state_.preview_text.clear();
        state_.preview_truncated = false;
        state_.preview_failed = false;
    } else {
        if (previous_preview_cleared) {
            state_.preview_kind = PreviewKind::None;
            state_.preview_path.clear();
            state_.preview_text.clear();
            state_.preview_truncated = false;
            state_.preview_failed = false;
        }
    }
    image_ticket_->Finish(current);
    image_ticket_.reset();
    pending_preview_path_.clear();
    pending_preview_generation_ = 0;
    if (current || previous_preview_cleared) NotifyStateChanged();
    return current;
}

void Controller::ClosePreviewState() {
    CancelPreviewRequest();
    state_.preview_generation = NextGeneration(state_.preview_generation);
    state_.preview_kind = PreviewKind::None;
    state_.preview_path.clear();
    state_.preview_text.clear();
    state_.preview_truncated = false;
    state_.preview_failed = false;
    NotifyStateChanged();
}

}  // namespace agent_ui::files
