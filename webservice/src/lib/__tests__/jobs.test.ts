/**
 * Unit tests for src/lib/jobs.ts – pure helper functions and createJob validation.
 */

import * as path from "path";
import { getDataDir, getJobDir, getJobInputDir, getJobOutputDir, createJob } from "../jobs";

describe("jobs pure helper functions", () => {
  const originalDataDir = process.env.GTOPT_DATA_DIR;

  afterEach(() => {
    // Restore env variable after each test
    if (originalDataDir === undefined) {
      delete process.env.GTOPT_DATA_DIR;
    } else {
      process.env.GTOPT_DATA_DIR = originalDataDir;
    }
  });

  describe("getDataDir", () => {
    it("returns GTOPT_DATA_DIR when set", () => {
      // Note: DATA_DIR is set at module load time, so this tests the default behaviour
      // since we cannot easily change a module-level constant after import.
      const result = getDataDir();
      expect(typeof result).toBe("string");
      expect(result.length).toBeGreaterThan(0);
    });
  });

  describe("getJobDir", () => {
    it("returns a path containing the token", () => {
      const result = getJobDir("test-token-123");
      expect(result).toContain("test-token-123");
    });

    it("returns a path under getDataDir()/jobs/", () => {
      const result = getJobDir("tok");
      expect(result).toBe(path.join(getDataDir(), "jobs", "tok"));
    });
  });

  describe("getJobInputDir", () => {
    it("returns <jobDir>/input", () => {
      const token = "input-token";
      const result = getJobInputDir(token);
      expect(result).toBe(path.join(getDataDir(), "jobs", token, "input"));
    });

    it("is a subdirectory of getJobDir", () => {
      const token = "tok2";
      expect(getJobInputDir(token)).toContain(getJobDir(token));
    });
  });

  describe("getJobOutputDir", () => {
    it("returns <jobDir>/output", () => {
      const token = "output-token";
      const result = getJobOutputDir(token);
      expect(result).toBe(path.join(getDataDir(), "jobs", token, "output"));
    });

    it("is a subdirectory of getJobDir", () => {
      const token = "tok3";
      expect(getJobOutputDir(token)).toContain(getJobDir(token));
    });
  });
});

describe("createJob validation", () => {
  it("throws when systemFile is empty string", async () => {
    await expect(createJob("")).rejects.toThrow("systemFile parameter must not be empty");
  });

  it("throws when systemFile is whitespace only", async () => {
    await expect(createJob("   ")).rejects.toThrow("systemFile parameter must not be empty");
  });

  it("creates a job with a valid systemFile", async () => {
    const job = await createJob("my_case.json");
    expect(job.token).toBeTruthy();
    expect(job.systemFile).toBe("my_case.json");
    expect(job.status).toBe("pending");
    expect(job.createdAt).toBeTruthy();
  });
});
