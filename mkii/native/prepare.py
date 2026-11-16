"""Generate the circuit and its initial state from the reference deck."""
from pathlib import Path
import subprocess
import sys

worker, circuit, output = (Path(p).resolve() for p in sys.argv[1:])
original = (circuit / "160_retrofit.cir").read_text(encoding="utf-8")
original = original.rsplit(".end", 1)[0].replace("THRESHV=0.1", "THRESHV=0.2", 1)
original = original.replace("SIN(0 0.5 1k)", "DC 0").replace("SIN(0 -0.5 1k)", "DC 0")
deck = circuit / "native_seed.cir"
deck.write_text(original + """
.options reltol=1e-5 abstol=1e-13 vntol=1e-9 method=gear
.control
set filetype=ascii
set num_threads=1
tran 2.5u .05 .04999 2.5u
write native_seed.raw all
quit
.endc
.end
""", encoding="utf-8")
subprocess.run([str(worker), "--batch", deck.name], cwd=circuit, check=True, timeout=180)
subprocess.run([sys.executable, str(Path(__file__).with_name("compile_deck.py")),
                str(circuit), str(circuit / "native_seed.raw"), str(output)], check=True)
