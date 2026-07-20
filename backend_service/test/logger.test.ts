import { describe, expect, it, vi } from "vitest";

import { createLogger } from "../src/logging/logger.js";

describe("结构化日志", () => {
  it("输出 JSON 并脱敏敏感字段", () => {
    const output = vi.spyOn(console, "log").mockImplementation(() => undefined);

    const logger = createLogger({
      env: "test",
      http: {
        host: "127.0.0.1",
        port: 3000
      },
      logging: {
        level: "debug"
      }
    });

    logger.info("测试日志", {
      request_id: "req-1",
      password: "secret"
    });

    expect(output).toHaveBeenCalledTimes(1);
    const line = output.mock.calls[0]?.[0];
    expect(typeof line).toBe("string");

    const parsed = JSON.parse(String(line));
    expect(parsed).toMatchObject({
      level: "info",
      message: "测试日志",
      request_id: "req-1",
      password: "[REDACTED]"
    });

    output.mockRestore();
  });
});
