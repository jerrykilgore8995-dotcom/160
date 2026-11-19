import pathlib, re, urllib.request
root=pathlib.Path(__file__).resolve().parent
sources={
'bjt':'https://raw.githubusercontent.com/kicad-spice-library/KiCad-Spice-Library/master/Models/Transistor/BJT/BJT.lib',
'jfet':'https://raw.githubusercontent.com/hanhha/SPICELIB/master/jfet.lib'}
data={k:urllib.request.urlopen(v,timeout=60).read().decode('latin1') for k,v in sources.items()}
def get(src, names, alias):
    lines=data[src].splitlines()
    for name in names:
        for i,l in enumerate(lines):
            if re.match(r'\s*\.model\s+'+re.escape(name)+r'\s+',l,re.I):
                block=l
                for n in lines[i+1:]:
                    if not n.strip():continue
                    if not n.lstrip().startswith('+'):break
                    block+=' '+n.lstrip()[1:]
                block=re.sub(r'\b(?:mfg|Vceo|Icrating|VCEO|ICRATING)\s*=\s*[^\s)]+','',block,flags=re.I)
                block=re.sub(r'(\.model\s+)\S+',r'\g<1>'+alias,block,count=1,flags=re.I)
                return '* archived '+name+' from '+sources[src]+'\n'+block
    raise RuntimeError('Missing '+str(names))
models=[get('bjt',['BC109C'],'LEG_N'),get('bjt',['BC179D','BC263C','BC179C','BC559C'],'LEG_P'),get('jfet',['J2N5457'],'LEG_J')]
# Diode model from existing 160 engineering candidate, not claimed manufacturer provenance.
models.append('.model LEG_D D(IS=2.52n N=1.73 RS=.568 CJO=4p TT=4n)')
(root/'devices.inc').write_text('\n'.join(models)+'\n')
print('\n'.join(models))
