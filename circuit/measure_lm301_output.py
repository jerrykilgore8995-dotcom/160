import pathlib, re, os, urllib.request, html
root=pathlib.Path(__file__).resolve().parent
os.chdir(root)
url='https://www.ltwiki.org/files/SPICE_MODEL_LIB-copyright.htm'
page=urllib.request.urlopen(url,timeout=60).read().decode('latin1')
plain=html.unescape(re.sub(r'<[^>]+>','',page))
model=re.search(r'(?ims)^\s*\.subckt\s+LM301A\s+1\s+2\s+3\s+4\s+5\s+6\s+7\s*$.*?^\s*\.ends[^\n]*',plain)
if not model: raise RuntimeError('Archived LM301A model not found')
text=model.group().strip()
assert '188.5E-6' in text and '21.22E6' in text
(root/'LM301A_archived.lib').write_text('* Source: '+url+'\n'+text+'\n',encoding='latin1')
base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]
old='XDRV DRIVER_IN DRVM VP15 VN15 DRV RC4558_SECTION'
assert base.count(old)==1
base=base.replace(old,'XDRV DRIVER_IN DRVM VP15 VN15 DRV OA6C1 OA6C2 LM301A\nC26 OA6C1 OA6C2 10p')
base=base.replace('.include RC4558.301','.include RC4558.301\n.include LM301A_archived.lib')
# Original measured circuit retained except OA6 and its external compensation.
# Rail and remaining output network are retrofit values, not original OA6 topology.
script=(root/'measure_limiting_frequency.py').read_text()
script=script.replace("base=(root/'160_retrofit.cir').read_text().rsplit('.end',1)[0]", "base="+repr(base))
exec(compile(script,str(root/'measure_limiting_frequency.py'),'exec'),{'__file__':str(root/'measure_limiting_frequency.py')})
