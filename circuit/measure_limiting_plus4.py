import os, pathlib, re, subprocess, csv, math
import numpy as np

root=pathlib.Path(__file__).resolve().parent
os.chdir(root)
out=root/'limiting_plus4'
out.mkdir(exist_ok=True)
target=os.environ.get('TARGET_GR','0')
base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]
assert 'SYM=0.499' in base
assert 'RT=909k CTVAL=22u' in (root/'modern_blackmer_cells.inc').read_text()
assert 'RLOAD OUT_PLUS 0 600' in base
base=re.sub(r'RATIO=[^ ]+','RATIO=120',base,count=1)
vin=.775*10**(.2)
vtarget=math.sqrt(600*.001*10**(.4))
peak=vin*math.sqrt(2)/2
base=base.replace('SIN(0 0.5 1k)',f'SIN(0 {peak} 1000)').replace('SIN(0 -0.5 1k)',f'SIN(0 {-peak} 1000)')
rows=[]
def run(th, makeup=0., step=5e-6, tag='cal'):
    name=f'{len(rows):03d}_{tag}'
    s=re.sub(r'THRESHV=[^ ]+',f'THRESHV={th:.12g}',base,count=1)
    s=re.sub(r'MAKEUPDB=[^ ]+',f'MAKEUPDB={makeup:.12g}',s,count=1)
    raw=out/(name+'.tsv'); deck=out/(name+'.cir')
    for attempt,(tol,dt,method) in enumerate([(1e-7,step,'trap'),(1e-6,min(step,2e-6),'gear'),(1e-6,min(step,1e-6),'gear')]):
        raw.unlink(missing_ok=True)
        deck.write_text(s+f"""
.options reltol={tol} abstol=1e-13 vntol=1e-9 method={method}
.control
set wr_singlescale
set wr_vecnames
tran {dt} 2 1.7 {dt}
wrdata {raw} v(out_plus) v(ec_minus)
quit
.endc
.end
""")
        with (out/(name+f'_{attempt}.log')).open('w') as log:
            subprocess.run(['ngspice','-b',str(deck)],stdout=log,stderr=subprocess.STDOUT)
        if raw.exists():
            a=np.loadtxt(raw,skiprows=1)
            if a.ndim==2 and a[-1,0]>1.99999: break
    else: raise RuntimeError('Incomplete simulation '+name)
    def fit(start,end):
        t=a[:,0]; sel=(t>=start)&(t<=end); t=t[sel]; y=a[sel,1]
        p=2*np.pi*1000*t
        X=np.column_stack([np.ones(len(t))]+[v for k in range(1,21) for v in (np.cos(k*p),np.sin(k*p))])
        w=np.sqrt(np.gradient(t)); b=np.linalg.lstsq(X*w[:,None],y*w,rcond=None)[0]
        return np.hypot(b[1::2],b[2::2])/np.sqrt(2)
    h=fit(1.9,2.0); early=fit(1.7,1.8)
    db=lambda x:20*np.log10(max(float(x),1e-30))
    row=dict(tag=tag,threshold_Vrms=th,makeup_control_dB=makeup,input_dBu=4.,output_Vrms=h[0],output_dBm=db(h[0]/math.sqrt(.6)),output_dBu=db(h[0]/.775),THD_percent=100*np.linalg.norm(h[1:])/h[0],settling_dB=db(h[0]/early[0]),solver=method,timestep=dt)
    for k in range(2,21):
        row[f'K{k}_dBc']=db(h[k-1]/h[0]);row[f'K{k}_dBu']=db(h[k-1]/.775);row[f'K{k}_dBm']=db(h[k-1]/math.sqrt(.6))
    rows.append(row)
    with (out/'all_trials.csv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
    print('MEASUREMENT',row,flush=True)
    raw.unlink()
    return row
ref=run(3,tag='reference')
endpoint=run(.01,tag='minimum_threshold')
gr=lambda r:20*math.log10(ref['output_Vrms']/r['output_Vrms'])
maximum=gr(endpoint)
print('MAXIMUM_GR_AT_10mV_THRESHOLD',maximum,flush=True)
wanted=maximum if target=='max' else float(target)
if wanted>maximum+.1:
    print('UNATTAINABLE',wanted,maximum,flush=True)
    raise SystemExit(0)
if target=='max': chosen=endpoint
elif wanted==0: chosen=ref
else:
    lo,hi=math.log(.01),math.log(3)
    chosen=endpoint
    for _ in range(12):
        x=(lo+hi)/2
        chosen=run(math.exp(x),tag='threshold_search')
        error=gr(chosen)-wanted
        if abs(error)<=.1: break
        if error>0:lo=x
        else:hi=x
    if abs(gr(chosen)-wanted)>.1:raise RuntimeError('GR tolerance missed')
# Makeup is applied inside the actual circuit, not post-normalized.
m=20*math.log10(vtarget/chosen['output_Vrms'])
for _ in range(8):
    final=run(chosen['threshold_Vrms'],m,tag='makeup_search')
    error=4-final['output_dBm']
    if abs(error)<=.05:break
    m+=error
else:raise RuntimeError('Output level tolerance missed')
# Refine the final spectrum with a smaller timestep.
fine=run(chosen['threshold_Vrms'],m,step=2e-6,tag='final_refinement')
final=fine
if abs(final['output_dBm']-4)>.05 or abs(final['settling_dB'])>.05:raise RuntimeError('Final output or settling tolerance missed')
final=dict(final,target_GR_dB=wanted,actual_GR_before_makeup_dB=gr(chosen),maximum_GR_dB=maximum,makeup_exceeds_20dB=abs(m)>20)
with (out/'summary.csv').open('w') as f:
    w=csv.DictWriter(f,fieldnames=final);w.writeheader();w.writerow(final)
print('FINAL_RESULT',final,flush=True)
