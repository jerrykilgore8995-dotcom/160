import pathlib,subprocess,numpy as np,csv
root=pathlib.Path(__file__).resolve().parent
out=root/'vca_datasheet';out.mkdir(exist_ok=True)
def run(name,dbv,gr,sym):
    peak=np.sqrt(2)*10**(dbv/20)
    # Datasheet current-input and I/V test arrangement: 20k input and feedback.
    s=f"""THAT2181B fixed control datasheet candidate
.include 2181_Macro_01.lib
.include RC4558.301
VCC VP 0 15
VEE VN 0 -15
VIN IN 0 SIN(0 {peak} 1000)
VEC EC 0 {gr*.0061}
RIN IN IIN 20k
RBIAS VNEG VN 5.1k
RT1 VP W {50000*(1-sym)}
RT2 W VN {50000*sym}
RSYM W SYM 220k
XVCA IIN 0 EC SYM VNEG 0 VP IOUT THAT2181B
RF IOUT OUT 20k
CF IOUT OUT 22p
XIV 0 IOUT VP VN OUT RC4558
.options reltol=1e-7 abstol=1e-14 vntol=1e-9
.control
set wr_singlescale
set wr_vecnames
tran .25u .08 .06 .25u
wrdata {out/name}.tsv v(in) v(out)
quit
.endc
.end
"""
    deck=out/(name+'.cir');deck.write_text(s)
    with (out/(name+'.log')).open('w') as log:subprocess.run(['ngspice','-b',str(deck)],cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
    a=np.loadtxt(out/(name+'.tsv'),skiprows=1);assert a[-1,0]>.07999
    t=a[:,0];w=np.sqrt(np.gradient(t));p=2*np.pi*1000*t
    X=np.column_stack([np.ones(len(t))]+[v for k in range(1,21) for v in (np.cos(k*p),np.sin(k*p))])
    b=np.linalg.lstsq(X*w[:,None],a[:,2]*w,rcond=None)[0]
    h=np.hypot(b[1::2],b[2::2]);d=lambda x:20*np.log10(max(float(x),1e-30))
    row=dict(case=name,input_dBV=dbv,commanded_GR_dB=gr,trim=sym,measured_gain_dB=d(h[0]/peak),THD_percent=100*np.linalg.norm(h[1:])/h[0])
    row.update({f'K{k}_dBc':d(h[k-1]/h[0]) for k in range(2,21)})
    print(row,flush=True);return row
def save(name,rows):
    with (out/name).open('w') as f:
        w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
if __name__=='__main__':
    trim=[run(f'trim{i}',0,0,x) for i,x in enumerate(np.linspace(.01,.99,21))]
    best=min(trim,key=lambda r:r['K2_dBc']);x=best['trim']
    fine=[run(f'fine{i}',0,0,v) for i,v in enumerate(np.linspace(max(.001,x-.049),min(.999,x+.049),21))]
    best=min(trim+fine,key=lambda r:r['K2_dBc'])
    save('trim.csv',trim+fine)
    rows=[run(f'spec{i}',v,g,best['trim']) for i,(v,g) in enumerate([(0,0),(10,0),(10,15),(-5,-15)])]
    save('datasheet.csv',rows)
