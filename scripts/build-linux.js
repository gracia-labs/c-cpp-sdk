import { mkdirSync, rmSync } from "node:fs";
import { join } from "node:path";

// The CPU of the build, x86_64 by default: bun scripts/build-linux.js [x86_64|aarch64]. Docker emulates a CPU that the
// host does not have.
const PLATFORMS = { x86_64: "linux/amd64", aarch64: "linux/arm64" };
const arch = process.argv[2] ?? "x86_64";
const platform = PLATFORMS[arch];
if (!platform) {
  console.error(`usage: bun scripts/build-linux.js [${Object.keys(PLATFORMS).join("|")}]`);
  process.exit(1);
}

const IMAGE = `gracia/c-cpp-sdk-linux:${arch}`;
const ZIP = `linux-${arch}.zip`;
const root = join(import.meta.dir, "..");
const outDir = join(root, "prebuilt");
const posix = (p) => p.replaceAll("\\", "/");

function docker(args) {
  const r = Bun.spawnSync(["docker", ...args], { stdout: "inherit", stderr: "inherit" });
  if (r.exitCode !== 0) process.exit(r.exitCode ?? 1);
}

docker(["build", "--platform", platform, "-f", join(root, "docker", "linux.Dockerfile"), "-t", IMAGE, join(root, "docker")]);

mkdirSync(outDir, { recursive: true });
rmSync(join(outDir, ZIP), { force: true });

// Source is read-only and the CMake tree lives on the container FS, so every build is clean.
const FILES = "libgracia_sdk.so desktop_viewer xr_viewer";
const script = [
  "cmake -S /sdk -B /tmp/build -DCMAKE_BUILD_TYPE=Release",
  "cmake --build /tmp/build --parallel",
  "cd /tmp/build/bin",
  "strip --strip-unneeded desktop_viewer xr_viewer",
  // One time for each file, and zip -X stores no other: two builds of one source write the same zip.
  `touch -d @315532800 ${FILES}`,
  `TZ=UTC zip -X -q /out/${ZIP} ${FILES}`,
].join(" && ");

docker([
  "run", "--rm", "--platform", platform,
  "-v", `${posix(root)}:/sdk:ro`,
  "-v", `${posix(outDir)}:/out`,
  IMAGE,
  "bash", "-c", script,
]);

console.log(`Wrote ${join(outDir, ZIP)}`);
