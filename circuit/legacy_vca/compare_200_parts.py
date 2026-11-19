import os,pathlib,re,subprocess,math
import numpy as np
root=pathlib.Path(__file__).resolve().parents[1];os.chdir(root)
out=root/'legacy_vca'/'parts_results';out.mkdir(exist_ok=True)
base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]
vendor=(root/'2181_Macro_01.lib').read_text(encoding='latin1')
devices=(root/'legacy_vca/devices.inc').read_text()
mode=os.environ.get('MODE','pair')
models={}
for name in ['LEG_N','LEG_P']:
    models[name]=re.search(r'^\.model\s+'+name+r'\s+.*$',devices,re.M|re.I).group()
# Change only active gain-cell devices Q3-Q8 initially. Keep sensing servo,
# current mirrors and bias hardware working; this is a hybrid, not a dbx200.
def replace(m):
    s=m.group()
    targets={'npn':['Q5','Q6'],'pnp':['Q7','Q8'],'pair':['Q5','Q6','Q7','Q8'],'extended':['Q3','Q4','Q5','Q6','Q7','Q8']}[mode]
    for q in targets:
        s=re.sub(r'(?im)^('+q+r'\s+\S+\s+\S+\s+\S+\s+)Q([NP])PNOM\s*$',lambda x:x[1]+('LEG_N' if x[2].upper()=='N' else 'LEG_P'),s)
    return s.replace('.ENDS', '\n'.join(models.values())+'\n.ENDS')
modified=re.sub(r'(?ims)^\.SUBCKT THAT2181B\b.*?^\.ENDS[^\n]*',replace,vendor,count=1)
assert modified!=vendor and 'Q5         8 4 27 LEG_N' in modified if mode=='pair' else modified!=vendor
# Modified vendor macro remains temporary; do not publish derived library.
path=root/'legacy_vca'/'temporary_modified.lib';path.write_text(modified,encoding='latin1')
def run(deck, tag, freq,th,makeup):
    peak=.775*10**.2/math.sqrt(2)
    deck=deck.replace('SIN(0 0.5 1k)',f'SIN(0 {peak} {freq})').replace('SIN(0 -0.5 1k)',f'SIN(0 {-peak} {freq})')
    for key,value in [('RATIO',120),('THRESHV',th),('MAKEUPDB',makeup)]:
        deck=re.sub(key+r'=[^ ]+',f'{key}={value}',deck,count=1)
    raw=out/(tag+'.tsv')
    # Do not retain decks containing modified vendor models.
    cir=root/'legacy_vca'/'temporary_test.cir'
    cir.write_text(deck+f"""
.options reltol=1e-6 abstol=1e-13 method=gear
.control
set wr_singlescale
set wr_vecnames
tran 5u 0.6 0.5 5u
wrdata {raw} v(out_plus) v(xblack.vca_in) v(xblack.vca_out) v(ec_minus)
quit
.endc
.end
""")
    with (out/(tag+'.log')).open('w') as log:subprocess.run(['ngspice','-b',str(cir)],stdout=log,stderr=subprocess.STDOUT,timeout=300)
    if not raw.exists():raise RuntimeError('No simulation '+tag)
    a=np.loadtxt(raw,skiprows=1)
    if a.ndim!=2 or a[-1,0]<.59999:raise RuntimeError('Incomplete '+tag)
    t=a[:,0];p=2*np.pi*freq*t;w=np.sqrt(np.gradient(t));X=np.column_stack([np.ones(len(t))]+[z for k in range(1,21) for z in (np.cos(k*p),np.sin(k*p))])
    b=np.linalg.lstsq(X*w[:,None],a[:,1]*w,rcond=None)[0];h=np.hypot(b[1::2],b[2::2])/math.sqrt(2)
    db=lambda x:20*math.log10(max(float(x),1e-30))
    result=dict(out_dBu=db(h[0]/.775),in_dc=float(np.mean(a[:,2])),out_node_dc=float(np.mean(a[:,3])),ec=float(np.mean(a[:,4])),THD=100*np.linalg.norm(h[1:])/max(h[0],1e-30),harmonics_dBc=[db(v/max(h[0],1e-30)) for v in h[1:]])
    print('PARTS_RESULT',mode,tag,result,flush=True);raw.unlink();return result
try:
    modern=run(base,'modern',1000,3,0)
    hybrid=base.replace('.include 2181_Macro_01.lib','.include legacy_vca/temporary_modified.lib')
    idle=run(hybrid,'idle',1000,3,0)
    comp=run(hybrid,'compression',1000,.1,0)
    boost=run(hybrid,'makeup',1000,3,10)
    moderncomp=run(base,'modern_compression',1000,.1,0)
    if abs(idle['out_dBu']-modern['out_dBu'])>1 or abs(idle['in_dc'])>.05 or abs(idle['out_node_dc'])>.05 or abs((idle['out_dBu']-comp['out_dBu'])-(modern['out_dBu']-moderncomp['out_dBu']))>1 or abs(boost['out_dBu']-idle['out_dBu']-10)>1:
        raise SystemExit('NOT_VALIDATED: hybrid electrical response outside tolerance')
    print('HYBRID_GATE_PASSED',mode,flush=True)
    # Exact existing limiting measurement algorithm, at requested frequency and GR.
    script=(root/'measure_limiting_frequency.py').read_text()
    script=script.replace("base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]", "base="+repr(hybrid))
    exec(compile(script,str(root/'measure_limiting_frequency.py'),'exec'),{'__file__':str(root/'measure_limiting_frequency.py')})
finally:
    path.unlink(missing_ok=True)
    (root/'legacy_vca'/'temporary_test.cir').unlink(missing_ok=True)
