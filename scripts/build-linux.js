import { mkdirSync, rmSync } from "node:fs";
import { join } from "node:path";

const IMAGE = "gracia/c-cpp-sdk-linux";
const root = join(import.meta.dir, "..");
const outDir = join(root, "prebuilt");
const posix = (p) => p.replaceAll("\\", "/");

function docker(args) {
  const r = Bun.spawnSync(["docker", ...args], { stdout: "inherit", stderr: "inherit" });
  if (r.exitCode !== 0) process.exit(r.exitCode ?? 1);
}

docker(["build", "-f", join(root, "docker", "linux.Dockerfile"), "-t", IMAGE, join(root, "docker")]);

mkdirSync(outDir, { recursive: true });
rmSync(join(outDir, "linux-x86_64.zip"), { force: true });

// Source is read-only and the CMake tree lives on the container FS, so every build is clean.
const script = [
  "cmake -S /sdk -B /tmp/build -DCMAKE_BUILD_TYPE=Release",
  "cmake --build /tmp/build --parallel",
  "cd /tmp/build/bin",
  "strip --strip-unneeded win_desktop_demo win_openxr_demo",
  "zip -q /out/linux-x86_64.zip libgracia_sdk.so win_desktop_demo win_openxr_demo",
].join(" && ");

docker([
  "run", "--rm",
  "-v", `${posix(root)}:/sdk:ro`,
  "-v", `${posix(outDir)}:/out`,
  IMAGE,
  "bash", "-c", script,
]);

console.log(`Wrote ${join(outDir, "linux-x86_64.zip")}`);
