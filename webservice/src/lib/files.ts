import { promises as fs } from "fs";
import path from "path";

/**
 * Recursively list all files under a directory, returning paths relative to
 * the base directory.
 */
export async function listDirRecursive(dir: string, base?: string): Promise<string[]> {
  const root = base ?? dir;
  const entries = await fs.readdir(dir, { withFileTypes: true });
  const files: string[] = [];
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      files.push(...(await listDirRecursive(full, root)));
    } else {
      files.push(path.relative(root, full));
    }
  }
  return files;
}
