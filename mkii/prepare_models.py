"""Prepare the verbatim vendor files and the two equivalent test decks."""
import hashlib
from pathlib import Path
import re
import shutil
import sys
from zipfile import ZipFile

circuit = Path(__file__).resolve().parents[1] / "circuit"
dest, ti, that = map(Path, sys.argv[1:])
dest.mkdir(parents=True, exist_ok=True)
(dest / "models").mkdir(exist_ok=True)
for archive, members in [(ti, ["RC4558.301"]), (that, ["2181_Macro_01.lib", "4301_Macro_01.lib"])]:
    with ZipFile(archive) as sources:
        for name in members:
            (dest / name).write_bytes(sources.read(name))
for name in ["modern_blackmer_cells.inc", "models/BC341_user.lib", "models/BC361_user.lib"]:
    shutil.copyfile(circuit / name, dest / name)
(dest / "supply_generated.inc").write_text(
    "".join(f"V{rail} {rail} 0 DC {volts}\n" for rail, volts in
            [("VP15", 15), ("VN15", -15), ("VP24", 24), ("VN24", -24)]),
    encoding="ascii")

original = (circuit / "160_retrofit.cir").read_text(encoding="utf-8")
(dest / "160_retrofit.cir").write_text(original, encoding="utf-8")
deck = original

def replace_line(pattern, replacement):
    global deck
    deck, count = re.subn(r"(?m)^" + pattern + "$", replacement, deck)
    if count != 1:
        raise ValueError(f"Expected a single canonical line for {pattern}, found {count}")

replace_line(r"VINP IN_PLUS 0 SIN\(.*\)", "VINP IN_PLUS 0 DC 0 EXTERNAL")
replace_line(r"VINM IN_MINUS 0 SIN\(.*\)", "VINM IN_MINUS 0 DC 0 EXTERNAL")
replace_line(r"\.end", ".options reltol=1e-5 abstol=1e-13 vntol=1e-9 method=gear\n"
                        ".save v(out_plus) v(ec_minus) v(level)\n.end")
(dest / "160_stream.cir").write_text(deck, encoding="utf-8")
(dest / "model-sha256.txt").write_text(
    "".join(f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.relative_to(dest).as_posix()}\n"
            for f in sorted(dest.rglob("*")) if f.is_file()
            and f.suffix.lower() in {".301", ".lib", ".inc", ".cir"}), encoding="ascii")
