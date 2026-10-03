"""Dump the named exports of Max Payne 2's engine DLLs, for reverse engineering.

Usage:
    pip install pefile
    python tools/dump_exports.py "C:/Program Files (x86)/Steam/steamapps/common/Max Payne 2 The Fall of Max Payne"

Writes one <dll>.txt per DLL to re/exports/ ("RVA mangled-name" per line) and prints a
per-class summary. re/ is git-ignored: these are derived from the game's binaries.
"""

import collections
import pathlib
import re
import sys

import pefile

SKIP = ("mfc", "msvc", "oleacc", "eax", "binkw32")


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)

    game = pathlib.Path(sys.argv[1])
    out = pathlib.Path(__file__).resolve().parent.parent / "re" / "exports"
    out.mkdir(parents=True, exist_ok=True)

    classes = collections.Counter()
    for dll in sorted(game.glob("*.dll")):
        if dll.name.lower().startswith(SKIP):
            continue
        pe = pefile.PE(str(dll), fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        if not hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            continue

        lines = []
        for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            name = sym.name.decode() if sym.name else f"#{sym.ordinal}"
            lines.append(f"{sym.address:08X} {name}")
            if m := re.match(r"\?[^@]+@([A-Za-z_]\w*)@@", name):
                classes[m.group(1)] += 1

        (out / f"{dll.name}.txt").write_text("\n".join(lines) + "\n")
        print(f"{dll.name:30} {len(lines):5} exports")

    print("\nMost-exported classes:")
    for name, count in classes.most_common(40):
        print(f"  {count:4}  {name}")


if __name__ == "__main__":
    main()
