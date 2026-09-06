#!/usr/bin/env python3
"""Offline revision-21 checks, TS wire fixtures and real Bad Apple frame replay.

python3 patches/host/run_vector_tests.py [--out /tmp/g2-vectors] [--no-sanitize]
No Bluetooth connection or firmware flashing. Needs cc, Python, bun and the
installed demos dependencies. Output is retained for visual inspection.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(*args, cwd=ROOT):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--no-sanitize", action="store_true")
    opts = parser.parse_args()
    out = opts.out.resolve() if opts.out else Path(tempfile.mkdtemp(prefix="g2-vectors-"))
    out.mkdir(parents=True, exist_ok=True)
    host = out / "vector_host_test"
    flags = [] if opts.no_sanitize else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    run("cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function", *flags,
        "-Ipatches", "-o", host, "patches/host/vector_host_test.c")
    run(host, out)
    demos = ROOT / "demos"
    run("bun", "test", "vector-protocol.test.ts", "svg-video.test.ts", cwd=demos)
    run("bun", "node_modules/typescript/bin/tsc", "-p", "tsconfig.vector.json", cwd=demos)
    run("bun", "vector-suite.ts", "--dump", out / "suite.bin", cwd=demos)
    run(host, out, out / "suite.bin")
    run("bun", "vector-suite.ts", "--bad-apple", "--gif", "bad_apple_quarter.gif", "--dump", out / "bad-apple.bin",
        "--svg-out", out / "svg", cwd=demos)
    run(host, out, out / "bad-apple.bin")
    smooth = demos / ".cache/bad-apple/smooth/video.json"
    if smooth.exists():
        run("bun", "vector-suite.ts", "--svg-video", smooth, "--dump", out / "smooth.bin",
            "--svg-out", out / "smooth-svg", cwd=demos)
        smooth_out = out / "smooth"
        smooth_out.mkdir(exist_ok=True)
        run(host, smooth_out, out / "smooth.bin")
    else:
        print("SKIP: prepared YouTube SVG video absent; run demos/prepare-bad-apple.ts to include it", flush=True)
    # The same freestanding compiler/relocation checks used by firmware generation.
    with (out / "thumb-build.json").open("w") as target:
        subprocess.run(["python3", "patches/build.py", "patches/patches_main.c", "--json"],
                       cwd=ROOT, stdout=target, check=True)
    blob = json.loads((out / "thumb-build.json").read_text())
    print(f"PASS: host + sanitizers + TypeScript + fixtures + 300 video frames; ARM blob {blob['text_len']} bytes")
    print(f"Artifacts: {out}")


if __name__ == "__main__":
    main()
