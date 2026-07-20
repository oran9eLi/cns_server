#include "core/persistence/dirty_state.hpp"

#include <algorithm>
#include <utility>

namespace cns::persistence {

namespace {

void MarkField(auto& field, bool selected, std::uint64_t revision) {
  if (selected) {
    field.dirty_revision = std::max(field.dirty_revision, revision);
  }
}

void RestoreField(auto& field, bool selected, std::uint64_t revision) {
  if (!selected) {
    return;
  }
  if (field.in_flight_revision <= revision) {
    field.in_flight_revision = 0;
  }
  field.dirty_revision = std::max(field.dirty_revision, revision);
}

bool FieldHasWork(const auto& field) {
  return field.dirty_revision != 0 || field.in_flight_revision != 0;
}

}  // namespace

void DirtyState::Mark(DesiredDeviceWrite write) {
  const auto vendor_id = write.record.vendor_id;
  auto [it, inserted] = entries_.try_emplace(
      vendor_id, Entry{.record = write.record,
                       .revision = write.revision,
                       .metadata = {},
                       .status = {},
                       .telemetry = {},
                       .urgency = Urgency::kTelemetryBatch,
                       .marked_at = std::chrono::steady_clock::now()});
  auto& entry = it->second;
  if (!inserted && write.revision >= entry.revision) {
    entry.record = std::move(write.record);
    entry.revision = write.revision;
  }
  MarkField(entry.metadata, write.write_metadata, write.revision);
  MarkField(entry.status, write.write_status, write.revision);
  MarkField(entry.telemetry, write.write_telemetry, write.revision);
  if (write.urgency == Urgency::kImmediate) {
    entry.urgency = Urgency::kImmediate;
  }
}

std::vector<DesiredDeviceWrite> DirtyState::TakeImmediate() {
  return TakeMatching(true, {}, {});
}

std::vector<DesiredDeviceWrite> DirtyState::TakeTelemetryDue(
    std::chrono::steady_clock::time_point now, std::chrono::seconds interval) {
  return TakeMatching(false, now, interval);
}

void DirtyState::Complete(std::string_view vendor_id, std::uint64_t revision) {
  const auto it = entries_.find(std::string{vendor_id});
  if (it == entries_.end()) {
    return;
  }
  auto complete = [revision](FieldState& field) {
    if (field.in_flight_revision <= revision) {
      field.in_flight_revision = 0;
    }
  };
  complete(it->second.metadata);
  complete(it->second.status);
  complete(it->second.telemetry);
  if (!HasWork(it->second)) {
    entries_.erase(it);
  }
}

void DirtyState::Restore(DesiredDeviceWrite write) {
  const auto vendor_id = write.record.vendor_id;
  auto [it, inserted] = entries_.try_emplace(
      vendor_id, Entry{.record = write.record,
                       .revision = write.revision,
                       .metadata = {},
                       .status = {},
                       .telemetry = {},
                       .urgency = Urgency::kTelemetryBatch,
                       .marked_at = std::chrono::steady_clock::now()});
  auto& entry = it->second;
  if (!inserted && write.revision >= entry.revision) {
    entry.record = std::move(write.record);
    entry.revision = write.revision;
  }
  RestoreField(entry.metadata, write.write_metadata, write.revision);
  RestoreField(entry.status, write.write_status, write.revision);
  RestoreField(entry.telemetry, write.write_telemetry, write.revision);
  if (write.urgency == Urgency::kImmediate) {
    entry.urgency = Urgency::kImmediate;
  }
}

std::size_t DirtyState::Size() const { return entries_.size(); }

std::vector<DesiredDeviceWrite> DirtyState::TakeMatching(
    bool immediate, std::chrono::steady_clock::time_point now,
    std::chrono::seconds interval) {
  std::vector<DesiredDeviceWrite> result;
  for (auto& [vendor_id, entry] : entries_) {
    static_cast<void>(vendor_id);
    if (!HasDirty(entry)) {
      continue;
    }
    const bool is_immediate = entry.urgency == Urgency::kImmediate;
    if (immediate != is_immediate ||
        (!immediate && now - entry.marked_at < interval)) {
      continue;
    }
    result.push_back({.record = entry.record,
                      .revision = entry.revision,
                      .write_metadata = entry.metadata.dirty_revision != 0,
                      .write_status = entry.status.dirty_revision != 0,
                      .write_telemetry = entry.telemetry.dirty_revision != 0,
                      .urgency = entry.urgency});
    auto dispatch = [](FieldState& field) {
      field.in_flight_revision =
          std::max(field.in_flight_revision, field.dirty_revision);
      field.dirty_revision = 0;
    };
    dispatch(entry.metadata);
    dispatch(entry.status);
    dispatch(entry.telemetry);
    entry.urgency = Urgency::kTelemetryBatch;
  }
  return result;
}

bool DirtyState::HasDirty(const Entry& entry) {
  return entry.metadata.dirty_revision != 0 ||
         entry.status.dirty_revision != 0 ||
         entry.telemetry.dirty_revision != 0;
}

bool DirtyState::HasWork(const Entry& entry) {
  return FieldHasWork(entry.metadata) || FieldHasWork(entry.status) ||
         FieldHasWork(entry.telemetry);
}

}  // namespace cns::persistence
