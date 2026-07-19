import re
import sys
from pathlib import Path


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')

RULES = {
    "cmdline": ("daemon/",),
    "discovery": ("daemon/", "fuse/", "lib/"),
    "fuse": ("daemon/",),
    "daemon": ("fuse/",),
    "lib": ("daemon/",),
    "include": ("daemon/", "fuse/", "lib/"),
}


def normalized_include(name):
    while name.startswith("../"):
        name = name[3:]
    if name.startswith("./"):
        name = name[2:]
    return name


def main():
    source_root = Path(sys.argv[1])
    violations = []

    for component, forbidden_prefixes in RULES.items():
        component_root = source_root / component

        for path in sorted(component_root.rglob("*")):
            if path.suffix not in {".c", ".h"}:
                continue

            for line_number, line in enumerate(
                    path.read_text(encoding="utf-8").splitlines(), 1):
                match = INCLUDE_RE.match(line)

                if not match:
                    continue

                included = normalized_include(match.group(1))

                if included.startswith(forbidden_prefixes):
                    relative_path = path.relative_to(source_root)
                    violations.append(
                        f"{relative_path}:{line_number}: forbidden include "
                        f"{match.group(1)}"
                    )

    if violations:
        print("\n".join(violations), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
