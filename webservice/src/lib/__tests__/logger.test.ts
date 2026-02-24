/**
 * Unit tests for src/lib/logger.ts
 * Tests pure functions that do not perform I/O.
 */

// Isolate module so side-effects (startup logging, LOG_DIR reads) reset between tests.
jest.mock("fs", () => {
  const actual = jest.requireActual<typeof import("fs")>("fs");
  return {
    ...actual,
    promises: {
      ...actual.promises,
      mkdir: jest.fn().mockResolvedValue(undefined),
      appendFile: jest.fn().mockResolvedValue(undefined),
      readFile: jest.fn().mockResolvedValue("line1\nline2\nline3\n"),
    },
    existsSync: jest.fn().mockReturnValue(false),
    readFileSync: jest.fn().mockReturnValue(""),
  };
});

describe("logger", () => {
  const originalEnv = process.env;

  beforeEach(() => {
    jest.resetModules();
    process.env = { ...originalEnv };
    delete process.env.GTOPT_LOG_DIR;
  });

  afterEach(() => {
    process.env = originalEnv;
  });

  describe("createLogger", () => {
    it("returns an object with info/warn/error/debug methods", async () => {
      const { createLogger } = await import("../logger");
      const log = createLogger("test");
      expect(typeof log.info).toBe("function");
      expect(typeof log.warn).toBe("function");
      expect(typeof log.error).toBe("function");
      expect(typeof log.debug).toBe("function");
    });

    it("calls console.log for info level", async () => {
      const { createLogger } = await import("../logger");
      const spy = jest.spyOn(console, "log").mockImplementation(() => {});
      const log = createLogger("test-component");
      log.info("hello world");
      expect(spy).toHaveBeenCalledWith(expect.stringContaining("[INFO]"));
      expect(spy).toHaveBeenCalledWith(expect.stringContaining("[test-component]"));
      expect(spy).toHaveBeenCalledWith(expect.stringContaining("hello world"));
      spy.mockRestore();
    });

    it("calls console.warn for warn level", async () => {
      const { createLogger } = await import("../logger");
      const spy = jest.spyOn(console, "warn").mockImplementation(() => {});
      const log = createLogger("test-component");
      log.warn("something wrong");
      expect(spy).toHaveBeenCalledWith(expect.stringContaining("[WARN]"));
      spy.mockRestore();
    });

    it("calls console.error for error level", async () => {
      const { createLogger } = await import("../logger");
      const spy = jest.spyOn(console, "error").mockImplementation(() => {});
      const log = createLogger("test-component");
      log.error("boom");
      expect(spy).toHaveBeenCalledWith(expect.stringContaining("[ERROR]"));
      spy.mockRestore();
    });

    it("calls console.log for debug level", async () => {
      const { createLogger } = await import("../logger");
      const spy = jest.spyOn(console, "log").mockImplementation(() => {});
      const log = createLogger("test-component");
      log.debug("detail");
      expect(spy).toHaveBeenCalledWith(expect.stringContaining("[DEBUG]"));
      spy.mockRestore();
    });

    it("message includes ISO timestamp", async () => {
      const { createLogger } = await import("../logger");
      const spy = jest.spyOn(console, "log").mockImplementation(() => {});
      const log = createLogger("ts-test");
      log.info("ts check");
      const msg: string = spy.mock.calls[spy.mock.calls.length - 1][0];
      // Should contain an ISO-like timestamp pattern [2025-…]
      expect(msg).toMatch(/\[\d{4}-\d{2}-\d{2}T/);
      spy.mockRestore();
    });
  });

  describe("getLogFilePath", () => {
    it("returns empty string when GTOPT_LOG_DIR is not set", async () => {
      delete process.env.GTOPT_LOG_DIR;
      const { getLogFilePath } = await import("../logger");
      expect(getLogFilePath()).toBe("");
    });

    it("returns log file path when GTOPT_LOG_DIR is set", async () => {
      process.env.GTOPT_LOG_DIR = "/tmp/logs";
      const { getLogFilePath } = await import("../logger");
      const result = getLogFilePath();
      expect(result).toContain("gtopt-webservice.log");
      expect(result).toContain("/tmp/logs");
    });
  });

  describe("readLogTail", () => {
    it("returns empty array when no log directory is configured", async () => {
      delete process.env.GTOPT_LOG_DIR;
      const { readLogTail } = await import("../logger");
      const lines = await readLogTail(10);
      expect(lines).toEqual([]);
    });

    it("returns last N lines when log file exists", async () => {
      process.env.GTOPT_LOG_DIR = "/tmp/logs";
      const fs = await import("fs");
      (fs.promises.readFile as jest.Mock).mockResolvedValue(
        "line1\nline2\nline3\nline4\nline5\n"
      );
      const { readLogTail } = await import("../logger");
      const lines = await readLogTail(3);
      expect(lines).toEqual(["line3", "line4", "line5"]);
    });

    it("returns empty array when readFile throws", async () => {
      process.env.GTOPT_LOG_DIR = "/tmp/logs";
      const fs = await import("fs");
      (fs.promises.readFile as jest.Mock).mockRejectedValue(new Error("ENOENT"));
      const { readLogTail } = await import("../logger");
      const lines = await readLogTail(10);
      expect(lines).toEqual([]);
    });
  });
});
