import pathlib,re,os,subprocess
import numpy as np
root=pathlib.Path(__file__).resolve().parent
os.chdir(root)
out=root/'driver_spec_results';out.mkdir(exist_ok=True)
vendor=(root/'RC4558.301').read_text(encoding='latin1').replace('\x1a','')
m=re.search(r'(?ims)^\.subckt RC4558\b.*?^\.ends[^\n]*',vendor)
assert m
s=m.group().replace('RC4558','OA6_301_TARGET',1)
# Boyle-model first-pass engineering targets, not a transistor-level LM301A.
# gm ~188.5uS; tail current / dominant cap ~0.5 V/us;
# gm/(2*pi*C2) ~1MHz. BF sets nominal input bias near 70nA.
changes={'C1':'2.0p','C2':'30p','IEE':'DC 15u','RC1':'5.305k','RC2':'5.305k','RE1':'2.45k','RE2':'2.45k','VLP':'DC 20','VLN':'DC 20'}
for k,v in changes.items():
    s,n=re.subn(r'(?im)^('+k+r'\s+\S+\s+\S+)(?:\s+.*)$',lambda m:m[1]+' '+v,s,count=1)
    assert n==1,k
s,n=re.subn(r'(?im)^(GA\s+\S+\s+\S+\s+\S+\s+\S+)[ \t]+[^\r\n]+',lambda m:m.group(1)+' 188.5u',s,count=1)
assert n==1
s=s.replace('BF=121.4','BF=107.1')
# Set systematic differential input offset; untouched external circuit.
s=re.sub(r'(?im)^Q2\s+12\s+1\s+14\s+QX','Q2 12 OFFIN 14 QX\nVOS OFFIN 1 DC 2m',s)
(root/'oa6_301_target.lib').write_text('* Modified TI Boyle model, experimental LM301A target, 2026-11-19\n'+s+'\n')
base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]
hybrid=base.replace('.include RC4558.301','.include RC4558.301\n.include oa6_301_target.lib').replace('XDRV DRIVER_IN DRVM VP15 VN15 DRV RC4558_SECTION','XDRV DRIVER_IN DRVM VP15 VN15 DRV OA6_301_TARGET')
assert hybrid.count('XDRV DRIVER_IN DRVM VP15 VN15 DRV OA6_301_TARGET')==1
assert 'XDRV DRIVER_IN DRVM VP15 VN15 DRV RC4558_SECTION' not in hybrid
assert '.include oa6_301_target.lib' in hybrid
assert re.search(r'(?im)^GA\s+6\s+0\s+11\s+12\s+188.5u',s)
assert 'BF=107.1' in s
print('DRIVER_MODEL_VERIFIED',next(x for x in hybrid.splitlines() if x.startswith('XDRV')),flush=True)
print('DRIVER_TARGET_PARAMETERS',changes,flush=True)
if os.environ.get('TEST')=='response':
    for label,deck in [('stock',base),('target',hybrid)]:
        deck=re.sub(r'THRESHV=[^ ]+','THRESHV=3',deck,count=1)
        deck=deck.replace('SIN(0 0.5 1k)','DC 0 AC 0.5').replace('SIN(0 -0.5 1k)','DC 0 AC 0.5 180')
        raw=out/(label+'.tsv');cir=out/(label+'.cir')
        cir.write_text(deck+f"""
.control
set wr_singlescale
set wr_vecnames
op
ac dec 100 0.1 20000
wrdata {raw} db(v(out_plus)) ph(v(out_plus))
quit
.endc
.end
""")
        with (out/(label+'.log')).open('w') as log:subprocess.run(['ngspice','-b',str(cir)],stdout=log,stderr=subprocess.STDOUT,check=True)
        a=np.loadtxt(raw,skiprows=1)
        assert a.ndim==2 and a[-1,0]>19000
        ref=np.interp(1000,a[:,0],a[:,1])
        for f in [.1,1,5,10,20,30,50,100,200,500,1000,2000,5000,10000,15000,20000]:
            print('DRIVER_RESPONSE',label,f,float(np.interp(f,a[:,0],a[:,1])),float(np.interp(f,a[:,0],a[:,1])-ref),float(np.interp(f,a[:,0],a[:,2])*180/np.pi),flush=True)
    # DC is blocked by coupling capacitors, so frequency response at exactly zero is zero.
else:
    script=(root/'measure_limiting_frequency.py').read_text()
    needle="base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]"
    assert script.count(needle)==1
    script=script.replace(needle,'base='+repr(hybrid))
    assert 'OA6_301_TARGET' in script
    exec(compile(script,str(root/'measure_limiting_frequency.py'),'exec'),{'__file__':str(root/'measure_limiting_frequency.py')})
