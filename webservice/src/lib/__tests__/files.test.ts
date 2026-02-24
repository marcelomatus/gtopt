/**
 * Unit tests for src/lib/files.ts
 */

import * as fs from "fs";
import * as path from "path";
import * as os from "os";
import { listDirRecursive } from "../files";

describe("listDirRecursive", () => {
  let tmpDir: string;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "gtopt-files-test-"));
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it("returns empty array for an empty directory", async () => {
    const result = await listDirRecursive(tmpDir);
    expect(result).toEqual([]);
  });

  it("lists a single flat file", async () => {
    fs.writeFileSync(path.join(tmpDir, "file.txt"), "content");
    const result = await listDirRecursive(tmpDir);
    expect(result).toEqual(["file.txt"]);
  });

  it("lists files in nested subdirectories", async () => {
    const sub = path.join(tmpDir, "a", "b");
    fs.mkdirSync(sub, { recursive: true });
    fs.writeFileSync(path.join(tmpDir, "root.txt"), "");
    fs.writeFileSync(path.join(sub, "deep.txt"), "");

    const result = await listDirRecursive(tmpDir);
    // Sort for deterministic comparison
    expect(result.sort()).toEqual(["a/b/deep.txt", "root.txt"].sort());
  });

  it("returns paths relative to the provided base directory", async () => {
    const sub = path.join(tmpDir, "sub");
    fs.mkdirSync(sub);
    fs.writeFileSync(path.join(sub, "data.csv"), "");

    // When base is explicitly the tmpDir, relative paths start from there
    const result = await listDirRecursive(sub, tmpDir);
    expect(result).toEqual(["sub/data.csv"]);
  });

  it("lists multiple files at the same level", async () => {
    ["a.txt", "b.txt", "c.txt"].forEach((name) =>
      fs.writeFileSync(path.join(tmpDir, name), "")
    );
    const result = await listDirRecursive(tmpDir);
    expect(result.sort()).toEqual(["a.txt", "b.txt", "c.txt"]);
  });
});
