import pathlib, subprocess, re, numpy as np, csv
from concurrent.futures import ThreadPoolExecutor
root=pathlib.Path(__file__).resolve().parent
out=root/'measurements'; out.mkdir(exist_ok=True)
base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]
def measure(case,threshold,freq,ratio=4,step=2e-6):
    s=re.sub(r'THRESHV=[^ ]+',f'THRESHV={threshold}',base)
    s=re.sub(r'RATIO=[^ ]+',f'RATIO={ratio}',s,count=1)
    s=s.replace('SIN(0 0.5 1k)',f'SIN(0 0.5 {freq})').replace('SIN(0 -0.5 1k)',f'SIN(0 -0.5 {freq})')
    duration=max(1.5,20/freq)
    start=duration-10/freq
    dt=min(step,1/(freq*160))
    raw=out/(case+'.tsv')
    deck=out/(case+'.cir')
    deck.write_text(s+f"""
.options reltol=1e-6 abstol=1e-13 vntol=1e-9
.control
set wr_singlescale
set wr_vecnames
tran {dt} {duration} {start} {dt}
wrdata {raw} v(in_plus) v(in_minus) v(oa3out) v(level) v(ec_minus) v(audio_out) v(out_plus)
quit
.endc
.end
""")
    with (out/(case+'.log')).open('w') as log:
        subprocess.run(['ngspice','-b',str(deck)],cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
    a=np.loadtxt(raw,skiprows=1)
    if a[-1,0]<duration-dt*2: raise RuntimeError(case+' incomplete')
    # Time-weighted simultaneous harmonic least-squares, avoiding adaptive-grid FFT.
    t=a[:,0]; keep=t>=start; t=t[keep]; y=a[keep]
    ph=2*np.pi*freq*t
    X=np.column_stack([np.ones(len(t))]+[v for k in range(1,21) for v in (np.cos(k*ph),np.sin(k*ph))])
    w=np.sqrt(np.maximum(np.gradient(t),0))
    b=np.linalg.lstsq(X*w[:,None],y[:,[3,4,5,6,7]]*w[:,None],rcond=None)[0]
    h=np.hypot(b[1::2],b[2::2]); fund=h[0,-1]
    db=lambda v:20*np.log10(max(float(v),1e-30))
    res=dict(case=case,threshold_Vrms=threshold,frequency_Hz=freq,ratio=ratio,input_Vrms=0.5*2**.5,output_Vrms=fund/2**.5,gain_dB=db(fund),phase_deg=np.degrees(np.arctan2(b[1,-1],b[2,-1])),control_DC_V=b[0,2],detector_DC_V=b[0,1],output_DC_V=b[0,-1],THD_percent=100*np.linalg.norm(h[1:,-1])/max(fund,1e-30))
    res.update({f'K{k}_dBc':db(h[k-1,-1]/max(fund,1e-30)) for k in range(2,21)})
    raw.unlink()
    print(res,flush=True)
    return res
def batch(spec,name):
    with ThreadPoolExecutor(max_workers=2) as ex: rows=list(ex.map(lambda x:measure(*x),spec))
    with (out/name).open('w') as f:
        writer=csv.DictWriter(f,fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
    return rows
if __name__=='__main__':
    thresholds=[.01,.03,.1,.3,1,3]
    rows=batch([(f'th_{i}',v,1000) for i,v in enumerate(thresholds)],'threshold.csv')
    gains=np.array([r['gain_dB'] for r in rows]); gr=gains[-1]-gains
    expected=.75*np.maximum(0,20*np.log10((1/np.sqrt(2))/thresholds))
    print('MEASURED GR',gr.tolist(),'EXPECTED GR',expected.tolist(),flush=True)
    # Only proceed if threshold causes monotonic, substantial gain reduction.
    if gr[0]<10 or np.any(np.diff(gains)<-.25):
        raise SystemExit('Threshold operation gate FAILED; inspect control reference and polarity')
    batch([(f'fr_{i}_{j}',v,f) for i,v in enumerate([3,.1]) for j,f in enumerate([20,30,50,100,200,500,1000,2000,5000,10000,15000,20000])],'frequency_harmonics.csv')
    batch([(f'ratio_{i}',.1,1000,r) for i,r in enumerate([1,2,4,10,120])],'ratio.csv')
    batch([('convergence_coarse',.1,1000,4,2e-6),('convergence_medium',.1,1000,4,1e-6),('convergence_fine',.1,1000,4,.5e-6)],'convergence.csv')
