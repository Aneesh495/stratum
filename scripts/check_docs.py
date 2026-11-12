#!/usr/bin/env python3
"""
scripts/check_docs.py - Verifies documentation structure, links, and style rules.
Ensures no em dashes are present in public documentation per project guidelines.
"""

import os
import re
import sys
from pathlib import Path
from urllib.parse import unquote

from acceptance_features import BINDINGS, parse_register
from acceptance_schema import RUNS

DOCS_DIR = "docs"
REQUIRED_DOCS = [
    "README.md",
    "docs/BUILD_STATUS.md",
    "docs/ACCEPTANCE_PLAN.md",
    "docs/spec/boot-contract.md",
    "docs/spec/memory-layout.md",
    "docs/spec/user-abi.md",
    "docs/spec/storage-format.md",
    "docs/spec/network-contract.md",
    "docs/spec/trace-format.md",
]


def check_file(filepath):
    errors = []
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    # Rule: avoid em dashes (\u2014 or &mdash;) in public prose
    if "\u2014" in content:
        lines = content.splitlines()
        for idx, line in enumerate(lines, 1):
            if "\u2014" in line:
                errors.append(f"{filepath}:{idx}: contains forbidden em dash character")

    # Link check for relative markdown links [text](path)
    links = re.findall(r'\[([^\]]+)\]\(([^)]+)\)', content)
    base_dir = os.path.dirname(filepath)
    for text, link in links:
        if link.startswith("http://") or link.startswith("https://") or link.startswith("#") or link.startswith("mailto:"):
            continue
        link_path = unquote(link.split("#")[0])
        if not link_path:
            continue
        target = os.path.normpath(os.path.join(base_dir, link_path))
        if not os.path.exists(target):
            errors.append(f"{filepath}: broken relative link to {link} (target {target} not found)")

    return errors


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    all_errors = []

    print("Checking required documentation files...")
    for doc in REQUIRED_DOCS:
        if not os.path.exists(doc):
            all_errors.append(f"Missing required documentation: {doc}")
        else:
            errs = check_file(doc)
            all_errors.extend(errs)

    # Also check any other .md files in docs/
    for dirpath, _, filenames in os.walk(DOCS_DIR):
        for fname in filenames:
            if fname.endswith(".md"):
                fpath = os.path.join(dirpath, fname)
                if fpath not in REQUIRED_DOCS:
                    errs = check_file(fpath)
                    all_errors.extend(errs)

    try:
        parse_register(Path("docs/ACCEPTANCE_PLAN.md").read_text(encoding="utf-8"))
        for identity, (paths, workloads) in BINDINGS.items():
            if any(not Path(path).is_file() for path in paths.split()) or any(
                name not in RUNS for name in workloads.split()
            ):
                all_errors.append(f"{identity}: missing bound source or workload")
    except (OSError, ValueError) as error:
        all_errors.append(str(error))

    diagram_root = Path("docs/diagrams")
    if (diagram_root / "INDEX.md").is_file():
        sources = {path.stem for path in (diagram_root / "source").glob("*.dot")}
        exports = {path.stem for path in (diagram_root / "rendered").glob("*.svg")}
        if len(sources) < 16 or sources != exports:
            all_errors.append("expected 16 or more matched DOT sources and SVG exports")

    if all_errors:
        print(f"\n[FAIL] Found {len(all_errors)} documentation issues:")
        for err in all_errors:
            print(f"  - {err}")
        return 1
    else:
        print(f"[OK] All documentation files verified. No em dashes or broken relative links found.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
