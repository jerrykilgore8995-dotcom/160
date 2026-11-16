"""Generate ngspice supply include and noise PWL files.
Run from circuit directory: python generate_supply.py --seconds 4
Requires numpy. No circuit analysis is performed.
Noise levels interpreted as dBV (1 V reference): broadband RMS, hum peak.
This is a rail-noise model, not output-referred noise. The schematic also
requires a negative 24 V rail for the output driver/protection.
"""
import argparse
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser()
p.add_argument("--seconds",type=float,default=4)
p.add_argument("--rate",type=int,default=192000)
p.add_argument("--seed",type=int,default=160)
p.add_argument("--quiet",action="store_true")
a=p.parse_args()
if a.seconds<=0 or a.rate<56000: p.error("positive duration and rate >= 56000 required")
n=int(a.seconds*a.rate)+1
t=np.arange(n)/a.rate
f=np.fft.rfftfreq(n,1/a.rate)
rng=np.random.default_rng(a.seed)
def pink():
    w=np.fft.rfft(rng.standard_normal(n))
    h=np.zeros_like(f)
    q=f>0
    h[q]=(1/np.sqrt(f[q]))/np.sqrt(1+(40/f[q])**2)/np.sqrt(1+(f[q]/14000)**2)
    x=np.fft.irfft(w*h,n=n)
    return x*(10**(-96/20)/np.sqrt(np.mean(x*x)))
lines=["* Fixed supply, approved simplified variant; noise in volts relative to 1V.", "* For deterministic THD use --quiet."]
for name,volts in [("VP15",15),("VN15",-15),("VP24",24),("VN24",-24)]:
    if a.quiet:
        lines.append(f"V{name} {name} 0 DC {volts}")
        continue
    x=pink()+10**(-82/20)*np.sin(2*np.pi*60*t)+10**(-91/20)*np.sin(2*np.pi*120*t)
    # Inline PWL continuation lines avoid simulator-specific PWL FILE syntax.
    lines.append(f"V{name} {name} 0 PWL(")
    lines.extend(f"+ {ti:.12g} {volts+xi:.12g}" for ti,xi in zip(t,x))
    lines.append("+ )")
Path("supply_generated.inc").write_text("\n".join(lines)+"\n")
