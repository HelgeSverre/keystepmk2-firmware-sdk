"""Compare two exported decompilations function by function, ignoring address shifts.

uv run python ghidra/scripts/verdiff.py 1.0.2 1.0.4 [--show N]

Functions are normalised (auto-generated names, addresses and local variable names replaced), hashed,
and the two sequences aligned. Prints how many are identical and each run that is not.
"""

import difflib
import hashlib
import re
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "out"


def load(version):
    text = (OUT / version / "decomp.c").read_text()
    funcs = []
    for m in re.finditer(r"// ===== (\S+) @ ([0-9a-f]+) =====\n(.*?)(?=\n// ===== |\Z)", text, re.DOTALL):
        body = re.sub(r"/\*.*?\*/", "", m.group(3), flags=re.DOTALL)
        body = re.sub(r"\n\s*\n", "\n", body)
        norm = re.sub(
            r"\b(FUN|DAT|LAB|PTR|UNK|_DAT|PTR_DAT|PTR_FUN|PTR_LAB|switchD|caseD|code_r0x|thunk_FUN)_[0-9a-f_]+\b",
            "SYM",
            body,
        )
        norm = re.sub(r"\b0x(080[0-9a-f]|2000|2001|2002)[0-9a-f]{4}\b", "ADDR", norm)
        norm = re.sub(
            r"\b[a-z]+Var\d+\b|\blocal_\w+\b|\b\w*Stack_\w+\b|\bextraout_\w+\b|\bunaff_\w+\b|\bin_\w+\b", "V", norm
        )
        funcs.append((m.group(1), m.group(2), norm, hashlib.md5(norm.encode()).hexdigest()))
    return funcs


def main():
    a, b = sys.argv[1], sys.argv[2]
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 0
    fa, fb = load(a), load(b)
    sm = difflib.SequenceMatcher(None, [f[3] for f in fa], [f[3] for f in fb], autojunk=False)
    same = sum(j2 - j1 for op, _, _, j1, j2 in sm.get_opcodes() if op == "equal")
    print(f"{a}: {len(fa)} functions, {b}: {len(fb)} functions, identical after normalising: {same}")
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == "equal":
            continue
        old = [f"{f[1]}({len(f[2])})" for f in fa[i1:i2]]
        new = [f"{f[1]}({len(f[2])})" for f in fb[j1:j2]]
        print(f"{op:8s} {a}: {' '.join(old) or '-'}\n         {b}: {' '.join(new) or '-'}")
        if show and i2 - i1 == j2 - j1:
            for x, y in zip(fa[i1:i2], fb[j1:j2]):
                d = [
                    line
                    for line in difflib.unified_diff(x[2].splitlines(), y[2].splitlines(), lineterm="", n=1)
                    if line[:1] in "+-" and line[:3] not in ("+++", "---")
                ]
                for line in d[:show]:
                    print("            " + line[:150])
                if len(d) > show:
                    print(f"            ... {len(d) - show} more changed lines")


main()
