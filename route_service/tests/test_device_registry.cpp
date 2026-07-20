#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/device/device_registry.hpp"

using namespace std::chrono_literals;

namespace {

using cns::device::DeviceRecord;
using cns::device::DeviceRegistry;
using cns::device::Status;

constexpr std::string_view kVendorA = "A1b2C3d4E5f6G7h8I9j0";
constexpr std::string_view kVendorB = "Z9y8X7w6V5u4T3s2R1q0";
constexpr std::string_view kVendorC = "M1n2B3v4C5x6Z7l8K9j0";
const auto kNow = std::chrono::sys_days{std::chrono::year{2026}/7/20} + 12h;

DeviceRecord Record(std::string_view vendor, std::int64_t school_id = 1,
                    std::optional<std::string> label = std::nullopt,
                    Status status = Status::kOffline) {
  return DeviceRecord{.vendor_id = std::string{vendor},
                      .school_id = school_id,
                      .school_name = school_id == 1 ? "SEU" : "Other",
                      .dcdw_label = std::move(label),
                      .model_version = "v1",
                      .status = status,
                      .last_seen_at = std::nullopt,
                      .latest_telemetry = std::nullopt,
                      .telemetry_received_at = std::nullopt,
                      .revision = 7};
}

}  // namespace

TEST_CASE("加载拒绝重复 vendor 且不留下部分目录") {
  DeviceRegistry registry;
  CHECK_FALSE(registry.Load({Record(kVendorA), Record(kVendorA)}));
  CHECK(registry.Find(kVendorA) == nullptr);
}

TEST_CASE("加载和新增拒绝同校重复角色号") {
  DeviceRegistry registry;
  CHECK_FALSE(registry.Load(
      {Record(kVendorA, 1, "DCDW-001"), Record(kVendorB, 1, "DCDW-001")}));
  REQUIRE(registry.Load({Record(kVendorA, 1, "DCDW-001")}));
  CHECK_FALSE(registry.AddProvisioned(Record(kVendorB, 1, "DCDW-001")));
  CHECK(registry.Find(kVendorB) == nullptr);
}

TEST_CASE("无效全量加载保持调用前合法目录和角色索引") {
  DeviceRegistry registry;
  REQUIRE(registry.Load({Record(kVendorA, 1, "DCDW-001")}));

  CHECK_FALSE(registry.Load({Record(kVendorB), Record(kVendorB)}));
  REQUIRE(registry.Find(kVendorA));
  CHECK(registry.Find(kVendorA)->dcdw_label == "DCDW-001");
  CHECK(registry.Find(kVendorB) == nullptr);
  CHECK_FALSE(registry.AddProvisioned(Record(kVendorC, 1, "DCDW-001")));

  CHECK_FALSE(registry.Load(
      {Record(kVendorB, 1, "DCDW-002"),
       Record(kVendorC, 1, "DCDW-002")}));
  REQUIRE(registry.Find(kVendorA));
  CHECK(registry.Find(kVendorA)->dcdw_label == "DCDW-001");
  CHECK(registry.Find(kVendorB) == nullptr);
}

TEST_CASE("同校多个空角色和不同学校相同角色均可共存") {
  DeviceRegistry registry;
  REQUIRE(registry.Load(
      {Record(kVendorA), Record(kVendorB),
       Record(kVendorC, 2, "DCDW-001")}));
  REQUIRE(registry.AddProvisioned(Record("P0o9I8u7Y6t5R4e3W2q1", 1,
                                         "DCDW-001")));
  CHECK(registry.Find(kVendorA));
  CHECK(registry.Find(kVendorB));
  CHECK(registry.Find(kVendorC));
}

TEST_CASE("启动加载保留 last_seen 并按原时间立即修正超时状态") {
  DeviceRegistry registry;
  auto record = Record(kVendorA, 1, std::nullopt, Status::kOnline);
  record.last_seen_at = kNow - 181s;
  REQUIRE(registry.Load({record}));
  const auto mutations = registry.ExpireInactive(kNow, 180s);
  REQUIRE(mutations.size() == 1);
  CHECK(mutations.front().reason == cns::state_event::ChangeReason::kActivityTimeout);
  CHECK(mutations.front().record.status == Status::kOffline);
  CHECK(mutations.front().record.last_seen_at == record.last_seen_at);
  CHECK(mutations.front().record.revision == 8);
}

TEST_CASE("registration 上下线且每次变化递增 revision") {
  DeviceRegistry registry;
  REQUIRE(registry.Load({Record(kVendorA)}));
  const cns::protocol::Registration online{std::string{kVendorA},
      cns::protocol::RegistrationStatus::kOnline, "SEU", "DCDW-001"};
  const auto up = registry.ApplyRegistration(online, kNow);
  REQUIRE(up);
  CHECK(up->reason == cns::state_event::ChangeReason::kRegistrationOnline);
  CHECK(up->record.status == Status::kOnline);
  CHECK(up->record.last_seen_at == kNow);
  CHECK(up->record.dcdw_label == "DCDW-001");
  CHECK(up->record.revision == 8);

  const cns::protocol::Registration offline{std::string{kVendorA},
      cns::protocol::RegistrationStatus::kOffline, std::nullopt, std::nullopt};
  const auto down = registry.ApplyRegistration(offline, kNow + 1s);
  REQUIRE(down);
  CHECK(down->reason == cns::state_event::ChangeReason::kRegistrationOffline);
  CHECK(down->record.status == Status::kOffline);
  CHECK(down->record.last_seen_at == kNow + 1s);
  CHECK(down->record.dcdw_label == "DCDW-001");
  CHECK(down->record.revision == 9);
}

TEST_CASE("学校不迁移且冲突角色号只拒绝字段") {
  DeviceRegistry registry;
  REQUIRE(registry.Load(
      {Record(kVendorA), Record(kVendorB, 1, "DCDW-001")}));
  const cns::protocol::Registration registration{std::string{kVendorA},
      cns::protocol::RegistrationStatus::kOnline, "Other", "DCDW-001"};
  const auto mutation = registry.ApplyRegistration(registration, kNow);
  REQUIRE(mutation);
  CHECK(mutation->record.school_name == "SEU");
  CHECK_FALSE(mutation->record.dcdw_label);
  CHECK(mutation->record.status == Status::kOnline);
  CHECK(mutation->record.last_seen_at == kNow);
  CHECK(mutation->record.revision == 8);
  REQUIRE(mutation->diagnostic);
  CHECK(mutation->diagnostic->find("拒绝设备自动迁移学校") !=
        std::string::npos);
  CHECK(mutation->diagnostic->find("拒绝同校冲突角色号") !=
        std::string::npos);
}

TEST_CASE("registration 换角色后释放旧索引供另一设备使用") {
  DeviceRegistry registry;
  REQUIRE(registry.Load(
      {Record(kVendorA, 1, "DCDW-001"), Record(kVendorB)}));
  const cns::protocol::Registration replace{std::string{kVendorA},
      cns::protocol::RegistrationStatus::kOnline, "SEU", "DCDW-002"};
  REQUIRE(registry.ApplyRegistration(replace, kNow));

  const cns::protocol::Registration claim_old{std::string{kVendorB},
      cns::protocol::RegistrationStatus::kOnline, "SEU", "DCDW-001"};
  const auto claimed = registry.ApplyRegistration(claim_old, kNow + 1s);
  REQUIRE(claimed);
  CHECK(claimed->record.dcdw_label == "DCDW-001");
  CHECK_FALSE(claimed->diagnostic);
}

TEST_CASE("telemetry 只补空角色号并保留完整 payload") {
  DeviceRegistry registry;
  REQUIRE(registry.Load({Record(kVendorA)}));
  const nlohmann::json first_payload{{"sensor", 42}, {"unknown", "kept"}};
  auto first = registry.ApplyTelemetry(kVendorA,
      cns::protocol::Telemetry{first_payload, "DCDW-001"}, kNow);
  REQUIRE(first);
  CHECK(first->reason == cns::state_event::ChangeReason::kTelemetry);
  CHECK(first->record.status == Status::kOnline);
  CHECK(first->record.dcdw_label == "DCDW-001");
  CHECK(first->record.latest_telemetry == first_payload);
  CHECK(first->record.telemetry_received_at == kNow);
  CHECK(first->record.last_seen_at == kNow);
  CHECK(first->record.revision == 8);

  const nlohmann::json second_payload{{"sensor", 43}};
  auto second = registry.ApplyTelemetry(kVendorA,
      cns::protocol::Telemetry{second_payload, "DCDW-002"}, kNow + 1s);
  REQUIRE(second);
  CHECK(second->record.dcdw_label == "DCDW-001");
  CHECK(second->record.latest_telemetry == second_payload);
  CHECK(second->record.revision == 9);
  CHECK_FALSE(second->diagnostic);
}

TEST_CASE("未知设备 registration 和 telemetry 均拒绝") {
  DeviceRegistry registry;
  const cns::protocol::Registration registration{std::string{kVendorA},
      cns::protocol::RegistrationStatus::kOnline, "SEU", std::nullopt};
  CHECK_FALSE(registry.ApplyRegistration(registration, kNow));
  CHECK_FALSE(registry.ApplyTelemetry(kVendorA,
      cns::protocol::Telemetry{nlohmann::json{{"sensor", 42}}, std::nullopt},
      kNow));
}

TEST_CASE("显式 offline 后 telemetry 活动恢复 online") {
  DeviceRegistry registry;
  REQUIRE(registry.Load(
      {Record(kVendorA, 1, std::nullopt, Status::kOnline)}));
  const cns::protocol::Registration offline{std::string{kVendorA},
      cns::protocol::RegistrationStatus::kOffline, std::nullopt, std::nullopt};
  const auto down = registry.ApplyRegistration(offline, kNow);
  REQUIRE(down);
  CHECK(down->reason == cns::state_event::ChangeReason::kRegistrationOffline);
  CHECK(down->record.status == Status::kOffline);

  auto mutation = registry.ApplyTelemetry(kVendorA,
      cns::protocol::Telemetry{nlohmann::json{{"sensor", 42}}, std::nullopt},
      kNow + 1s);
  REQUIRE(mutation);
  CHECK(mutation->record.status == Status::kOnline);
  CHECK(mutation->record.revision == 9);
}

TEST_CASE("超时只产生一次 mutation 且边界为大于等于") {
  DeviceRegistry registry;
  auto record = Record(kVendorA, 1, std::nullopt, Status::kOnline);
  record.last_seen_at = kNow - 180s;
  REQUIRE(registry.Load({record}));
  const auto first = registry.ExpireInactive(kNow, 180s);
  REQUIRE(first.size() == 1);
  CHECK(first.front().record.revision == 8);
  CHECK(registry.ExpireInactive(kNow + 1s, 180s).empty());
}

TEST_CASE("online 但无 last_seen 不参与超时") {
  DeviceRegistry registry;
  REQUIRE(registry.Load(
      {Record(kVendorA, 1, std::nullopt, Status::kOnline)}));
  CHECK(registry.ExpireInactive(kNow, 180s).empty());
  REQUIRE(registry.Find(kVendorA));
  CHECK(registry.Find(kVendorA)->status == Status::kOnline);
  CHECK(registry.Find(kVendorA)->revision == 7);
}

TEST_CASE("无设备超时时扫描保持记录地址稳定") {
  DeviceRegistry registry;
  auto record = Record(kVendorA, 1, std::nullopt, Status::kOnline);
  record.last_seen_at = kNow;
  REQUIRE(registry.Load({record}));
  const auto* before = registry.Find(kVendorA);
  REQUIRE(before);

  CHECK(registry.ExpireInactive(kNow + 179s, 180s).empty());
  CHECK(registry.Find(kVendorA) == before);
}

TEST_CASE("更新设备 A 不使设备 B 的 Find 指针失效") {
  DeviceRegistry registry;
  REQUIRE(registry.Load({Record(kVendorA), Record(kVendorB)}));
  const auto* device_b = registry.Find(kVendorB);
  REQUIRE(device_b);

  REQUIRE(registry.ApplyTelemetry(kVendorA,
      cns::protocol::Telemetry{nlohmann::json{{"sensor", 42}}, std::nullopt},
      kNow));
  CHECK(registry.Find(kVendorB) == device_b);
  CHECK(device_b->vendor_id == kVendorB);
}

TEST_CASE("telemetry 角色号冲突只拒绝补全") {
  DeviceRegistry registry;
  REQUIRE(registry.Load(
      {Record(kVendorA), Record(kVendorB, 1, "DCDW-001")}));
  const auto payload = nlohmann::json{{"sensor", 42}};
  auto mutation = registry.ApplyTelemetry(kVendorA,
      cns::protocol::Telemetry{payload, "DCDW-001"}, kNow);
  REQUIRE(mutation);
  CHECK_FALSE(mutation->record.dcdw_label);
  CHECK(mutation->record.latest_telemetry == payload);
  CHECK(mutation->record.status == Status::kOnline);
  CHECK(mutation->record.revision == 8);
  CHECK(mutation->diagnostic);
}
