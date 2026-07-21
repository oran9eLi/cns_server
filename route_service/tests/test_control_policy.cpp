#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/command/control_policy.hpp"
#include "core/command/source_request.hpp"

#include <array>
#include <string>
#include <variant>

namespace {

using cns::command::ControlCommand;
using cns::command::RejectedSourceRequest;
using cns::command::SourceControlRequest;
using cns::command::SourceKind;

constexpr auto kVendor = "A1b2C3d4E5f6G7h8I9j0";
constexpr auto kCommand = "550e8400-e29b-41d4-a716-446655440000";

SourceControlRequest ParseValid(std::string_view payload,
                                SourceKind kind = SourceKind::kHostApp) {
  auto result = cns::command::ParseSourceControlRequest(payload, kind);
  REQUIRE(std::holds_alternative<SourceControlRequest>(result));
  return std::get<SourceControlRequest>(std::move(result));
}

RejectedSourceRequest ParseRejected(
    std::string_view payload, SourceKind kind = SourceKind::kHostApp) {
  auto result = cns::command::ParseSourceControlRequest(payload, kind);
  REQUIRE(std::holds_alternative<RejectedSourceRequest>(result));
  return std::get<RejectedSourceRequest>(std::move(result));
}

std::string Ack(std::string_view body) {
  return std::string{"{\"command_id\":\""} + kCommand + "\"," +
         std::string{body} + "}";
}

}  // namespace

TEST_CASE("飞控topic严格区分来源请求和设备ACK") {
  const auto source = cns::command::ParseSourceControlRequestTopic(
      "cns_rpi", "cns_rpi/sources/web-console/control/request");
  REQUIRE(source.has_value());
  CHECK(*source == "web-console");

  const auto device = cns::command::ParseDeviceControlAckTopic(
      "cns_rpi", std::string{"cns_rpi/"} + kVendor + "/control/ack");
  REQUIRE(device.has_value());
  CHECK(*device == kVendor);

  for (const auto topic : {"cns_rpi/sources/web-console/control",
                           "cns_rpi/sources/web/console/control/request",
                           "cns_rpi/sources/web-console/config/request",
                           "other/sources/web-console/control/request"}) {
    CHECK_FALSE(cns::command::ParseSourceControlRequestTopic("cns_rpi", topic)
                    .has_value());
  }
  CHECK_FALSE(cns::command::ParseDeviceControlAckTopic(
                  "cns_rpi", std::string{"cns_rpi/"} + kVendor + "/config/ack")
                  .has_value());
}

TEST_CASE("四种飞控命令严格解析参数并保留幂等载荷") {
  const auto pwm = ParseValid(
      R"({"schema_version":1,"request_id":"r1","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[1000,1500,2000,1501]}})");
  CHECK(pwm.command == ControlCommand::kSetMotorPwm);
  REQUIRE(pwm.parameters.pwm_us.has_value());
  CHECK(*pwm.parameters.pwm_us ==
        std::array<std::uint16_t, 4>{1000, 1500, 2000, 1501});
  CHECK(pwm.comparison_payload.at("request_id") == "r1");

  for (const auto& [name, command] :
       std::array<std::pair<std::string_view, ControlCommand>, 3>{
           {{"emergency_stop", ControlCommand::kEmergencyStop},
            {"takeoff", ControlCommand::kTakeoff},
            {"land", ControlCommand::kLand}}}) {
    const auto request = ParseValid(
        std::string{"{\"schema_version\":1,\"request_id\":\"r-"} +
        std::string{name} + "\",\"target\":{\"vendor_id\":\"" + kVendor +
        "\"},\"command\":\"" + std::string{name} +
        "\",\"parameters\":{}}");
    CHECK(request.command == command);
    CHECK_FALSE(request.parameters.pwm_us.has_value());
  }
}

TEST_CASE("飞控命令拒绝非法PWM无参数命令附加字段和未知命令") {
  for (const auto payload : {
           R"({"schema_version":1,"request_id":"r1","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[999,1500,1500,1500]}})",
           R"({"schema_version":1,"request_id":"r2","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[1500,1500,1500,2001]}})",
           R"({"schema_version":1,"request_id":"r3","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[1500,1500,1500]}})",
           R"({"schema_version":1,"request_id":"r4","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[1500,1500,1500,1500,1500]}})",
           R"({"schema_version":1,"request_id":"r5","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[true,1500,1500,1500]}})",
           R"({"schema_version":1,"request_id":"r6","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[1500.0,1500,1500,1500]}})",
           R"({"schema_version":1,"request_id":"r7","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":["1500",1500,1500,1500]}})",
           R"({"schema_version":1,"request_id":"r8","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"set_motor_pwm","parameters":{"pwm_us":[1500,1500,1500,1500],"extra":1}})",
           R"({"schema_version":1,"request_id":"r9","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"takeoff","parameters":{"height":10}})"}) {
    CHECK(ParseRejected(payload).error.code == "invalid_parameters");
  }
  CHECK(ParseRejected(
            R"({"schema_version":1,"request_id":"r10","target":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"command":"hover","parameters":{}})")
            .error.code == "unsupported_command");
}

TEST_CASE("设备飞控下发只包含关联命令动作和参数") {
  CHECK(cns::command::BuildDeviceControlSet(
            kCommand, ControlCommand::kTakeoff, {}) ==
        nlohmann::json{{"command_id", kCommand},
                       {"command", "takeoff"},
                       {"parameters", nlohmann::json::object()}});
  CHECK(cns::command::ExpectedMavlinkCommand(ControlCommand::kSetMotorPwm) ==
        31013);
  CHECK(cns::command::ExpectedMavlinkCommand(ControlCommand::kEmergencyStop) ==
        31090);
  CHECK(cns::command::ExpectedMavlinkCommand(ControlCommand::kTakeoff) == 31091);
  CHECK(cns::command::ExpectedMavlinkCommand(ControlCommand::kLand) == 31092);
}

TEST_CASE("设备飞控ACK接受MAVLink终态进度和本地等待状态") {
  const auto accepted = cns::command::ParseDeviceControlAck(Ack(
      R"("command":"takeoff","status":"accepted","mavlink_command":31091,"result":0,"result_code":"accepted","future":1)"));
  REQUIRE(accepted.has_value());
  CHECK(accepted->business_status == "accepted");
  CHECK(accepted->raw.at("future") == 1);

  const auto progress = cns::command::ParseDeviceControlAck(Ack(
      R"("command":"takeoff","status":"in_progress","mavlink_command":31091,"result":5,"result_code":"in_progress","progress":30,"result_param2":-7)"));
  REQUIRE(progress.has_value());
  CHECK(progress->business_status == "in_progress");
  CHECK(progress->progress == 30);
  CHECK(progress->result_param2 == -7);

  const auto pending = cns::command::ParseDeviceControlAck(Ack(
      R"("command":"takeoff","status":"in_progress","mavlink_command":31091,"result_code":"pending")"));
  REQUIRE(pending.has_value());
  CHECK(pending->business_status == "in_progress");
  CHECK_FALSE(pending->result.has_value());
}

TEST_CASE("设备飞控ACK接受六类MAVLink拒绝和本地失败") {
  for (const auto& [result, code] :
       std::array<std::pair<int, std::string_view>, 6>{
           {{1, "temporarily_rejected"}, {2, "denied"},
            {3, "unsupported"}, {4, "failed"}, {6, "cancelled"},
            {7, "unknown_result"}}}) {
    const auto ack = cns::command::ParseDeviceControlAck(Ack(
        std::string{"\"command\":\"land\",\"status\":\"rejected\","
                    "\"mavlink_command\":31092,\"result\":"} +
        std::to_string(result) + ",\"result_code\":\"" +
        std::string{code} + "\""));
    REQUIRE(ack.has_value());
    CHECK(ack->business_status == "rejected");
  }

  for (const auto status : {"rejected", "timeout"}) {
    const auto ack = cns::command::ParseDeviceControlAck(Ack(
        std::string{"\"command\":\"emergency_stop\",\"status\":\""} +
        status + "\",\"error_code\":\"" +
        (std::string_view{status} == "timeout" ? "mcu_ack_timeout"
                                                : "command_busy") +
        "\""));
    REQUIRE(ack.has_value());
    CHECK(ack->business_status == status);
  }
}

TEST_CASE("设备飞控ACK拒绝已知字段类型或组合冲突") {
  for (const auto& payload : {
           Ack(R"("command":"takeoff","status":"accepted","mavlink_command":31090,"result":0,"result_code":"accepted")"),
           Ack(R"("command":"takeoff","status":"accepted","mavlink_command":31091,"result":1,"result_code":"accepted")"),
           Ack(R"("command":"takeoff","status":"in_progress","mavlink_command":31091,"result":5,"result_code":"in_progress","progress":101,"result_param2":0)"),
           Ack(R"("command":"takeoff","status":"in_progress","mavlink_command":31091,"result_code":"pending","progress":1)"),
           Ack(R"("command":"takeoff","status":"rejected","error_code":"Bad-Code")"),
           Ack(R"("command":"takeoff","status":"timeout","error_code":3)"),
           Ack(R"("command":"takeoff","status":"unknown","error_code":"x")")}) {
    CHECK_FALSE(cns::command::ParseDeviceControlAck(payload).has_value());
  }
}
