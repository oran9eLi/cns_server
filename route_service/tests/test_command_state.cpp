#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/command/command_state.hpp"

#include <array>
#include <chrono>
#include <string_view>

using namespace std::chrono_literals;

namespace {

using cns::command::CommandRecord;
using cns::command::CommandStatus;
using cns::command::CommandType;

const auto kAt = std::chrono::sys_days{std::chrono::year{2026}/7/20} + 18h +
                 30min + 123ms;

CommandRecord Record(CommandStatus status) {
  return {.command_id = "550e8400-e29b-41d4-a716-446655440000",
          .source_id = "web-console",
          .request_id = "req-001",
          .target_vendor_id = "A1b2C3d4E5f6G7h8I9j0",
          .request_payload = {{"schema_version", 1}, {"value", 2}},
          .status = status,
          .error_code = std::nullopt,
          .error_message = std::nullopt,
          .device_ack = std::nullopt,
          .created_at = kAt,
          .dispatched_at = std::nullopt,
          .updated_at = kAt,
          .completed_at = std::nullopt};
}

}  // namespace

TEST_CASE("命令类型使用稳定字符串往返并拒绝未知值") {
  CHECK(cns::command::ToString(CommandType::kConfig) == "config");
  CHECK(cns::command::ToString(CommandType::kControl) == "control");
  CHECK(cns::command::ParseCommandType("config") == CommandType::kConfig);
  CHECK(cns::command::ParseCommandType("control") == CommandType::kControl);
  CHECK_FALSE(cns::command::ParseCommandType("unknown").has_value());
}

TEST_CASE("飞控进度是非终态而投递不确定是终态") {
  CHECK_FALSE(cns::command::IsTerminal(CommandStatus::kInProgress));
  CHECK(cns::command::IsTerminal(CommandStatus::kDeliveryUncertain));
}

TEST_CASE("配置命令状态只允许规定方向迁移") {
  using cns::command::CanTransition;
  CHECK(CanTransition(CommandStatus::kPending, CommandStatus::kDispatched));
  CHECK(CanTransition(CommandStatus::kPending, CommandStatus::kFailed));
  CHECK(CanTransition(CommandStatus::kPending, CommandStatus::kTimeout));
  CHECK(CanTransition(CommandStatus::kDispatched, CommandStatus::kSucceeded));
  CHECK(CanTransition(CommandStatus::kDispatched, CommandStatus::kFailed));
  CHECK(CanTransition(CommandStatus::kDispatched, CommandStatus::kTimeout));

  for (const auto terminal : {CommandStatus::kSucceeded, CommandStatus::kFailed,
                              CommandStatus::kTimeout}) {
    CHECK(cns::command::IsTerminal(terminal));
    for (const auto desired : {CommandStatus::kPending, CommandStatus::kDispatched,
                               CommandStatus::kSucceeded, CommandStatus::kFailed,
                               CommandStatus::kTimeout}) {
      CHECK_FALSE(CanTransition(terminal, desired));
    }
  }
  CHECK_FALSE(cns::command::IsTerminal(CommandStatus::kPending));
  CHECK_FALSE(cns::command::IsTerminal(CommandStatus::kDispatched));
  CHECK_FALSE(CanTransition(CommandStatus::kPending, CommandStatus::kSucceeded));
  CHECK_FALSE(CanTransition(CommandStatus::kDispatched, CommandStatus::kPending));
  CHECK_FALSE(CanTransition(CommandStatus::kPending, CommandStatus::kPending));
}

TEST_CASE("UUID格式函数固定版本变体大小写和分隔符") {
  std::array<std::uint8_t, 16> bytes{};
  CHECK(cns::command::FormatUuidV4(bytes) ==
        "00000000-0000-4000-8000-000000000000");
  bytes.fill(0xff);
  CHECK(cns::command::FormatUuidV4(bytes) ==
        "ffffffff-ffff-4fff-bfff-ffffffffffff");
  const auto generated = cns::command::GenerateUuidV4();
  CHECK(generated.size() == 36);
  CHECK(generated[14] == '4');
  CHECK((generated[19] == '8' || generated[19] == '9' ||
         generated[19] == 'a' || generated[19] == 'b'));
}

TEST_CASE("幂等比较忽略对象键顺序但拒绝任何内容变化") {
  auto record = Record(CommandStatus::kPending);
  record.request_payload = nlohmann::json::parse(
      R"({"target":{"vendor_id":"A"},"parameters":{"a":1,"b":2}})");
  const auto reordered = nlohmann::json::parse(
      R"({"parameters":{"b":2,"a":1},"target":{"vendor_id":"A"}})");
  CHECK(cns::command::CompareRequest(record, reordered) ==
        cns::command::IdempotencyResult::kSame);
  auto changed = reordered;
  changed["parameters"]["a"] = 2;
  CHECK(cns::command::CompareRequest(record, changed) ==
        cns::command::IdempotencyResult::kConflict);
}

TEST_CASE("来源ACK覆盖进行中设备成功拒绝路由失败和超时") {
  const cns::command::ResolvedTarget target{
      "A1b2C3d4E5f6G7h8I9j0", "SEU", "DCDW-002"};
  for (const auto status : {CommandStatus::kPending, CommandStatus::kDispatched,
                            CommandStatus::kTimeout}) {
    auto record = Record(status);
    const auto ack = cns::command::BuildSourceAck(record, target, kAt);
    CHECK(ack["status"] == (status == CommandStatus::kPending
                                ? "pending"
                                : status == CommandStatus::kDispatched ? "dispatched"
                                                                        : "timeout"));
    CHECK(ack["business_status"].is_null());
    CHECK(ack["target"]["dcdw_label"] == "DCDW-002");
    CHECK(ack["occurred_at"] == "2026-07-20T18:30:00.123Z");
  }

  for (const auto business : {"applied", "already_applied"}) {
    auto record = Record(CommandStatus::kSucceeded);
    record.device_ack = {{"status", business}, {"restart_required", true}};
    const auto ack = cns::command::BuildSourceAck(record, target, kAt);
    CHECK(ack["status"] == "succeeded");
    CHECK(ack["business_status"] == business);
    CHECK(ack["device"]["restart_required"] == true);
    CHECK(ack["device"]["error_code"].is_null());
  }

  auto rejected = Record(CommandStatus::kFailed);
  rejected.error_code = "device_rejected";
  rejected.error_message = "设备拒绝配置";
  rejected.device_ack = {{"status", "rejected"},
                         {"restart_required", false},
                         {"error_code", "invalid_parameter"}};
  auto ack = cns::command::BuildSourceAck(rejected, target, kAt);
  CHECK(ack["business_status"] == "rejected");
  CHECK(ack["error"]["code"] == "device_rejected");
  CHECK(ack["device"]["error_code"] == "invalid_parameter");

  auto route_failed = Record(CommandStatus::kFailed);
  route_failed.target_vendor_id = std::nullopt;
  route_failed.error_code = "target_not_found";
  route_failed.error_message = "目标设备不存在";
  ack = cns::command::BuildSourceAck(route_failed, std::nullopt, kAt);
  CHECK(ack["business_status"].is_null());
  CHECK(ack["error"]["code"] == "target_not_found");
  CHECK_FALSE(ack.contains("target"));
}

TEST_CASE("持久化前拒绝使用空命令号和可空请求号") {
  const cns::command::ProtocolError error{"invalid_json", "请求不是合法JSON"};
  auto ack = cns::command::BuildPrePersistenceRejection("req-1", error, kAt);
  CHECK(ack["request_id"] == "req-1");
  CHECK(ack["command_id"].is_null());
  CHECK(ack["status"] == "failed");
  CHECK(ack["business_status"].is_null());
  CHECK(ack["error"]["code"] == "invalid_json");
  CHECK_FALSE(ack.contains("target"));

  ack = cns::command::BuildPrePersistenceRejection(std::nullopt, error, kAt);
  CHECK(ack["request_id"].is_null());
}
