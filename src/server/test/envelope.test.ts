import { expect, test, describe } from "bun:test";
import {
  envelopeError,
  bridgeOk,
  bridgeInner,
  bridgeErrorCode,
  bridgeErrorHint,
} from "../src/bridge/envelope.ts";

describe("envelope", () => {
  test("bridgeOk only true on status=success", () => {
    expect(bridgeOk({ status: "success" })).toBe(true);
    expect(bridgeOk({ status: "error" })).toBe(false);
    expect(bridgeOk(null)).toBe(false);
    expect(bridgeOk("nope")).toBe(false);
  });

  test("bridgeInner returns result object or empty", () => {
    expect(bridgeInner({ status: "success", result: { a: 1 } })).toEqual({ a: 1 });
    expect(bridgeInner({ status: "success" })).toEqual({});
    expect(bridgeInner({ status: "success", result: [1, 2] })).toEqual({});
    expect(bridgeInner(undefined)).toEqual({});
  });

  test("envelopeError coerces messages and carries code/hint", () => {
    expect(envelopeError("boom")).toEqual({ status: "error", error: "boom" });
    expect(envelopeError(new Error("kaboom"))).toEqual({
      status: "error",
      error: "kaboom",
    });
    const env = envelopeError("nope", { code: "asset_not_found", hint: "try x" });
    expect(env.error_code).toBe("asset_not_found");
    expect(env.error_hint).toBe("try x");
  });

  test("bridgeErrorCode narrows to the closed set", () => {
    expect(bridgeErrorCode({ status: "error", error_code: "asset_not_found" })).toBe(
      "asset_not_found",
    );
    expect(bridgeErrorCode({ status: "error", error_code: "made_up" })).toBeUndefined();
    expect(bridgeErrorHint({ status: "error", error_hint: "do y" })).toBe("do y");
  });
});
