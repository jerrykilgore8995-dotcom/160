"""Black-box check of the actual plug-in worker against an independent batch deck."""
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np

worker, deck, ngspice, report = [Path(p).resolve() for p in sys.argv[1:]]
rate = 44100
settle = 0.05
scale = 13.790836925122916
frames = 4410
grid = np.arange(frames) / rate
signal = (0.09 * np.sin(2 * np.pi * 1000 * grid)).astype("<f4")
threshold = float(np.float32(0.2))
ratio = 4.0
makeup = 0.0

def read_exact(stream, n):
    data = stream.read(n)
    if len(data) != n:
        raise RuntimeError(f"Worker returned {len(data)} of {n} requested bytes")
    return data

start = time.perf_counter()
rendered = []
with subprocess.Popen([str(worker), "."], cwd=deck, stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE) as proc:
    proc.stdin.write(struct.pack("<IId", 0x16000101, 0, rate))
    for i in range(0, frames, 256):
        block = signal[i:i + 256]
        proc.stdin.write(struct.pack("<Ifff", len(block), threshold, ratio, makeup))
        proc.stdin.write(block.tobytes())
        proc.stdin.flush()
        count, control = struct.unpack("<If", read_exact(proc.stdout, 8))
        assert count == len(block) and np.isfinite(control)
        rendered.append(np.frombuffer(read_exact(proc.stdout, count * 4), dtype="<f4"))
    proc.stdin.close()
    assert proc.wait(timeout=30) == 0
audio = np.concatenate(rendered)
worker_seconds = time.perf_counter() - start

original = (deck / "160_retrofit.cir").read_text(encoding="utf-8")
original = re.sub(r"THRESHV=\S+", f"THRESHV={threshold:.9g}", original, count=1)
for name, node, sign in [("VINP", "IN_PLUS", 1), ("VINM", "IN_MINUS", -1)]:
    values = sign * scale * signal.astype(float) / 2
    source = f"{name} {node} 0 PWL(0 0 {settle - 1/rate:.16g} 0\n"
    source += "".join(f"+ {t + settle:.16g} {v:.17g}\n" for t, v in zip(grid, values))
    original = re.sub(rf"(?m)^{name} .+$", source + "+ )", original)
batch = original.rsplit(".end", 1)[0] + """
.options reltol=1e-5 abstol=1e-13 vntol=1e-9 method=gear
.control
set num_threads=1
set numdgt=16
set wr_singlescale
tran 2.5u 0.15 0 2.5u
wrdata mkii-reference.dat v(out_plus)
quit
.endc
.end
"""
with tempfile.TemporaryDirectory(prefix="160-mkii-") as temp:
    temp_deck = Path(temp) / "reference.cir"
    temp_deck.write_text(batch, encoding="utf-8")
    result = subprocess.run([str(ngspice), "-b", str(temp_deck)], cwd=deck,
                            capture_output=True, timeout=180)
    raw = deck / "mkii-reference.dat"
    if result.returncode != 0 or not raw.exists():
        raise RuntimeError(result.stdout.decode(errors="replace")[-3000:] +
                           result.stderr.decode(errors="replace")[-3000:])
    reference = np.loadtxt(raw)
    raw.unlink()
    assert reference[-1, 0] >= settle + grid[-1]
target = np.interp(settle + grid, reference[:, 0], reference[:, 1]) / scale
error = audio - target

def spectrum(output):
    window = grid >= 0.075
    t = grid[window]
    columns = [np.ones_like(t)]
    columns += [v for k in range(1, 21) for v in
                (np.cos(2*np.pi*k*1000*t), np.sin(2*np.pi*k*1000*t))]
    b = np.linalg.lstsq(np.column_stack(columns), output[window], rcond=None)[0]
    h = b[1::2] - 1j * b[2::2]
    return {
        "fundamental_peak": abs(h[0]),
        "fundamental_phase_deg": float(np.angle(h[0], deg=True)),
        "k2_to_k20_dbc": {f"K{k}": 20*np.log10(abs(x/h[0]))
                          for k, x in enumerate(h[1:], 2)},
        "thd_percent": float(100*np.linalg.norm(h[1:])/abs(h[0])),
    }

rms = np.sqrt(np.mean(target**2))
results = {
    "case": {"rate_hz": rate, "sine_hz": 1000, "input_peak_dbfs": 20*np.log10(0.09),
             "threshold_vrms": threshold, "ratio": ratio, "makeup_db": makeup,
             "load_ohm": 600, "samples": frames},
    "worker_seconds": worker_seconds,
    "worker_wall_seconds_per_audio_second": worker_seconds / (frames/rate),
    "waveform_rmse_dBFS": 20*np.log10(np.sqrt(np.mean(error**2))),
    "waveform_peak_error_dBFS": 20*np.log10(np.max(np.abs(error))),
    "reference": spectrum(target),
    "worker": spectrum(audio),
}
report.write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
assert rms > 0.001 and np.all(np.isfinite(audio)), "Invalid reference or worker output"
assert np.sqrt(np.mean(error**2)) < 0.002*rms, "Waveform RMS error exceeds 0.2% of reference RMS"
assert abs(results["worker"]["thd_percent"] - results["reference"]["thd_percent"]) < 0.02
