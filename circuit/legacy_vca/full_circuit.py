import pathlib, os, re, subprocess, math
import numpy as np
root=pathlib.Path(__file__).resolve().parents[1]
os.chdir(root)
kind=os.environ.get('CELL','200')
out=root/'legacy_vca'/'full_results';out.mkdir(exist_ok=True)
base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]
cells=(root/'modern_blackmer_cells.inc').read_text()
# Retain the entire RMS split, detector, current-to-voltage converter and output stage.
# Interfaces are provisional, not validated replacements. No audio gain correction.
if kind=='200':
    wrapper=""".subckt BLACKMER_VCA IN OUT EC_MINUS VP VN REF params: SYM=0.499
VHALF V7 REF 7.5
* Experimental complementary control drive. Polarity checked by full-circuit gain sweep.
EPO PO REF EC_MINUS REF -1
ENO NO REF EC_MINUS REF 1
XLEG IN OUT REF PO REF NO V7 VN REF DBX200
.ends BLACKMER_VCA"""
elif kind=='202C':
    wrapper=""".subckt BLACKMER_VCA IN OUT EC_MINUS VP VN REF params: SYM=0.499
* 202C nominal -50mV/dB versus 2181 negative port -6.1mV/dB.
EEC EC REF EC_MINUS REF 8.1967213115
RSYM SYM REF 1Meg
XLEG IN OUT EC SYM VP VN REF DBX202C
.ends BLACKMER_VCA"""
else: raise ValueError(kind)
cells=re.sub(r'(?ims)^\.subckt BLACKMER_VCA\b.*?^\.ends BLACKMER_VCA[^\n]*',lambda m:wrapper,cells,count=1)
# Avoid name collision between the trim parameter and node name.
cells=cells.replace('RSYM SYM REF 1Meg','RSYM SYMN REF 1Meg').replace('EC SYM VP VN REF DBX202C','EC SYMN VP VN REF DBX202C')
(out/'cells.inc').write_text(cells)
base=base.replace('.include modern_blackmer_cells.inc','.include legacy_vca/devices.inc\n.include legacy_vca/dbx200.inc\n.include legacy_vca/dbx202c.inc\n.include legacy_vca/full_results/cells.inc')
base=re.sub(r'RATIO=[^ ]+','RATIO=120',base,count=1)
peak=.775*10**.2/math.sqrt(2)
base=base.replace('SIN(0 0.5 1k)',f'SIN(0 {peak} 1k)').replace('SIN(0 -0.5 1k)',f'SIN(0 {-peak} 1k)')
def measure(th, makeup, tag, deckbase):
    s=re.sub(r'THRESHV=[^ ]+',f'THRESHV={th}',deckbase,count=1)
    s=re.sub(r'MAKEUPDB=[^ ]+',f'MAKEUPDB={makeup}',s,count=1)
    raw=out/(tag+'.tsv');deck=out/(tag+'.cir')
    deck.write_text(s+f"""
.options reltol=1e-6 abstol=1e-12 method=gear
.control
set wr_singlescale
set wr_vecnames
tran 5u 0.5 0.4 5u
wrdata {raw} v(out_plus) v(ec_minus) v(xblack.vca_in) v(xblack.vca_out)
quit
.endc
.end
""")
    with (out/(tag+'.log')).open('w') as log:
        subprocess.run(['ngspice','-b',str(deck)],stdout=log,stderr=subprocess.STDOUT,timeout=240)
    if not raw.exists(): raise RuntimeError('No transient '+tag)
    a=np.loadtxt(raw,skiprows=1)
    if a.ndim!=2 or a[-1,0]<.49999:raise RuntimeError('Incomplete '+tag)
    t=a[:,0];y=a[:,1];w=np.sqrt(np.gradient(t));p=2*np.pi*1000*t
    x=np.column_stack([np.ones(len(t))]+[z for k in range(1,21) for z in (np.cos(k*p),np.sin(k*p))])
    b=np.linalg.lstsq(x*w[:,None],y*w,rcond=None)[0]
    h=np.hypot(b[1::2],b[2::2])/math.sqrt(2)
    result=dict(output_dBu=20*math.log10(max(h[0],1e-30)/.775),ec_dc=float(np.mean(a[:,2])),input_dc=float(np.mean(a[:,3])),output_node_dc=float(np.mean(a[:,4])),THD_diagnostic_only=100*np.linalg.norm(h[1:])/max(h[0],1e-30))
    print('FULL_CIRCUIT_DIAGNOSTIC',kind,tag,result,flush=True)
    return result
# Test modern control in identical fixture before comparing vintage gain.
modern=base.replace('.include legacy_vca/devices.inc\n.include legacy_vca/dbx200.inc\n.include legacy_vca/dbx202c.inc\n.include legacy_vca/full_results/cells.inc','.include modern_blackmer_cells.inc')
ref=measure(3,0,'modern_reference',modern)
idle=measure(3,0,'legacy_reference',base)
cut=measure(.1,0,'legacy_compressed',base)
gain=measure(3,10,'legacy_makeup',base)
if abs(idle['output_dBu']-ref['output_dBu'])>1 or abs(idle['input_dc'])>.05 or idle['output_dBu']-cut['output_dBu']<5 or abs(gain['output_dBu']-idle['output_dBu']-10)>1:
    raise SystemExit('NOT_VALIDATED: full circuit gain, bias or control response failed; harmonic comparison withheld')
print('ELECTRICAL_GATE_PASSED',flush=True)
# Only after electrical checks, reuse existing limiting measurement algorithm verbatim
# with the substituted full netlist in memory. Original files never overwritten.
script=(root/'measure_limiting_frequency.py').read_text()
script=script.replace("base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]", "base="+repr(base.replace(f'SIN(0 {peak} 1k)','SIN(0 0.5 1k)').replace(f'SIN(0 {-peak} 1k)','SIN(0 -0.5 1k)')))
exec(compile(script,str(root/'measure_limiting_frequency.py'),'exec'),{'__file__':str(root/'measure_limiting_frequency.py')})
