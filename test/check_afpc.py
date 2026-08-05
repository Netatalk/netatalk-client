#!/usr/bin/env python3
"""Black-box parser and invocation-name checks for afpc."""

import os
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=5)


def expect(command, code, text):
    result = run(command)
    if result.returncode != code or text not in result.stderr + result.stdout:
        print("unexpected result:", command, file=sys.stderr)
        print("stdout:", result.stdout, file=sys.stderr)
        print("stderr:", result.stderr, file=sys.stderr)
        print("return code:", result.returncode, file=sys.stderr)
        return False
    return True


def main():
    afpc, build = sys.argv[1:]
    ok = True
    ok &= expect([afpc], 2, "Usage:")
    ok &= expect([afpc, "discover-extra"], 2, "unknown command")
    ok &= expect([afpc, "sl", "status", "mountpoint"], 2, "Usage:")

    with tempfile.TemporaryDirectory() as directory:
        unknown = os.path.join(directory, "not-afpc")
        os.symlink(afpc, unknown)
        ok &= expect([unknown, "help"], 2, "unsupported invocation name")

        if build == "fuse":
            url = os.path.join(directory, "mount_afpfs")
            os.symlink(afpc, url)
            ok &= expect([afpc, "mount"], 2, "Try 'afpc fs mount'.")
            ok &= expect([afpc, "status"], 2,
                         "Try 'afpc fs status' or 'afpc sl status'.")
            ok &= expect([afpc, "fs", "status-extra"], 2, "afpc fs <command>")
            ok &= expect([url, "discover"], 2, "use 'afpc discover'")
        else:
            ok &= expect([afpc, "mount"], 2,
                         "unavailable because FUSE support was not built")
            ok &= expect([afpc, "status"], 2, "Try 'afpc sl status'.")
            ok &= expect([afpc, "fs", "status"], 2,
                         "namespace is unavailable because FUSE support was not built")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
