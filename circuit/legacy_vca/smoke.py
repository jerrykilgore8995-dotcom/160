import pathlib, subprocess, os
import numpy as np
root=pathlib.Path(__file__).resolve().parent
os.chdir(root)
out=root/'results';out.mkdir(exist_ok=True)
kind=os.environ.get('CELL','200')
for g in [0,-10,-20,10]:
    name=f'{kind}_{g}'
    if kind=='200':
        cell=f'VPP vp 0 7.5\nVNN vn 0 -15\nVPI pi 0 0\nVPO po 0 {g*.006}\nVNI ni 0 0\nVNO no 0 {-g*.006}\nXVC in out pi po ni no vp vn 0 DBX200'
    else:
        cell=f'VPP vp 0 15\nVNN vn 0 -15\nVEC ec 0 {-g*.05}\nVSYM sym 0 0\nXVC in out ec sym vp vn 0 DBX202C'
    deck=out/(name+'.cir')
    raw=out/(name+'.txt')
    deck.write_text(f"""Legacy VCA diagnostic, {kind} {g}dB
.include devices.inc
.include dbx200.inc
.include dbx202c.inc
{cell}
Vsig sig 0 SIN(0 0.5 1000)
Rin sig in 50k
VOUT out 0 0
.options reltol=1e-7 abstol=1e-13
.control
op
show all
set wr_singlescale
set wr_vecnames
tran 2u 0.1 0.08 2u
wrdata {raw} v(in) i(VOUT)
quit
.endc
.end
""")
    with (out/(name+'.log')).open('w') as log:
        subprocess.run(['ngspice','-b',str(deck)],stdout=log,stderr=subprocess.STDOUT)
    if not raw.exists():
        print('FAILED',name,flush=True);continue
    a=np.loadtxt(raw,skiprows=1)
    if a.ndim!=2 or a[-1,0]<.09999:print('INCOMPLETE',name,flush=True);continue
    t=a[:,0]; p=2*np.pi*1000*t; w=np.sqrt(np.gradient(t)); X=np.column_stack([np.ones(len(t))]+[z for k in range(1,21) for z in (np.cos(k*p),np.sin(k*p))])
    y=a[:,2]; b=np.linalg.lstsq(X*w[:,None],y*w,rcond=None)[0]; h=np.hypot(b[1::2],b[2::2]); gain=20*np.log10(max(h[0],1e-30)/10e-6)
    print('DIAGNOSTIC',name,'gain',gain,'input_DC',np.mean(a[:,1]),'output_DC',b[0],'THD',100*np.linalg.norm(h[1:])/max(h[0],1e-30),'harmonics_dBc',20*np.log10(np.maximum(h[1:]/max(h[0],1e-30),1e-30)),flush=True)
    if abs(gain-g)>1 or abs(np.mean(a[:,1]))>.1:print('NOT_VALIDATED',name,flush=True)
