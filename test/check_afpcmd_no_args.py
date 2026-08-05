#!/usr/bin/env python3

import subprocess
import sys


def main():
    try:
        result = subprocess.run(
            [sys.argv[1]],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except subprocess.TimeoutExpired:
        print("bare afpcmd did not exit", file=sys.stderr)
        return 1

    if result.returncode != 1:
        print(
            f"bare afpcmd exited {result.returncode}, expected 1",
            file=sys.stderr,
        )
        return 1

    if "--browse" not in result.stdout or "<afp url>" not in result.stdout:
        print("bare afpcmd did not print its usage", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
