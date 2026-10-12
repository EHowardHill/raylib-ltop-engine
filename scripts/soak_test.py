#!/usr/bin/env python3
"""Build-health soak test.

1. Generator determinism: bake twice, the generated code must be identical.
2. Save/load round trip: run the native binary with --soak (fresh game ->
   save -> reload -> re-save; every persisted byte must survive, except the
   rng state which legitimately advances across a room spawn).

Exit code 0 = healthy.
"""
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUTO = [os.path.join(ROOT, "include_auto"), os.path.join(ROOT, "src_auto")]


def hash_auto():
    digest = hashlib.sha256()
    for folder in sorted(AUTO):
        for filename in sorted(os.listdir(folder)):
            path = os.path.join(folder, filename)
            if os.path.isfile(path):
                digest.update(filename.encode())
                digest.update(open(path, "rb").read())
    return digest.hexdigest()


def run(cmd, **kwargs):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kwargs)


def main():
    failures = []

    # 1. Generator determinism.
    first = run([".venv/bin/python3", "scripts/generate_assets.py"])
    if first.returncode != 0:
        failures.append("generator failed: %s" % first.stderr)
    else:
        hash_one = hash_auto()
        run([".venv/bin/python3", "scripts/generate_assets.py"])
        hash_two = hash_auto()
        if hash_one != hash_two:
            failures.append("generator output churns between runs")

    # 2. Native build (the soak needs it).
    build = run(["./build-mac.sh"])
    if build.returncode != 0:
        failures.append("native build failed: %s" % build.stderr[-800:])
    else:
        for slot in ("save0.dat", "save1.dat", "save2.dat"):
            path = os.path.join(ROOT, slot)
            if os.path.exists(path):
                os.remove(path)
        soak = run(["./main", "--soak"])
        output = soak.stderr + soak.stdout
        if "SOAK OK" not in output:
            failures.append("save soak failed: %s" % output[-800:])

    if failures:
        for failure in failures:
            print("FAIL: %s" % failure)
        return 1
    print("soak test: all green (deterministic bake + save round trip)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
