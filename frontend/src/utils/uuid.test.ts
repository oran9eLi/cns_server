import { describe, expect, it, vi } from "vitest";

import { createUuidV4 } from "./uuid.js";

const UUID_V4_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

describe("createUuidV4", () => {
  it("uses native randomUUID when it is available", () => {
    const randomUUID = vi.fn(() => "123e4567-e89b-42d3-a456-426614174000");
    const getRandomValues = vi.fn();

    expect(createUuidV4({ randomUUID, getRandomValues })).toBe(
      "123e4567-e89b-42d3-a456-426614174000"
    );
    expect(randomUUID).toHaveBeenCalledOnce();
    expect(getRandomValues).not.toHaveBeenCalled();
  });

  it("generates a valid UUID v4 when randomUUID is unavailable", () => {
    const getRandomValues = <T extends ArrayBufferView | null>(array: T): T => {
      if (array instanceof Uint8Array) {
        array.fill(0);
      }
      return array;
    };

    const uuid = createUuidV4({ getRandomValues });

    expect(uuid).toBe("00000000-0000-4000-8000-000000000000");
    expect(uuid).toMatch(UUID_V4_PATTERN);
  });
});
