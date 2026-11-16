"""Lossless source inventory for MKII. No device substitution.
Run after downloading the same manufacturer archives as the SPICE reference.
This is the input audit for the compiler, not a simulator.
"""
import pathlib, re, json, hashlib, collections
root=pathlib.Path(__file__).resolve().parents[1]/'circuit'
files={}
def read(path):
    path=path.resolve()
    if path in files: return
    raw=path.read_bytes()
    lines=[]
    for number,line in enumerate(raw.decode('latin-1').splitlines(),1):
        s=line.strip()
        if not s or s.startswith('*'): continue
        if s.startswith('+'):
            if not lines: raise ValueError(f'{path}:{number}: orphan continuation')
            lines[-1]['text']+=' '+s[1:].strip()
        else: lines.append(dict(line=number,text=s))
    files[path]=dict(sha256=hashlib.sha256(raw).hexdigest(),lines=lines)
    for row in lines:
        m=re.match(r'\.include\s+["\x27]?([^"\x27\s]+)',row['text'],re.I)
        if m: read(path.parent/m[1])
read(root/'160_retrofit.cir')
records=[]; definitions={}; models=[]; scope='top'
for path,data in files.items():
    scope='top'
    for row in data['lines']:
        s=row['text']; tokens=s.split(); kind=tokens[0].lower()
        if path.name=='160_retrofit.cir' and row['line']==1: continue
        if kind=='.subckt':
            scope=tokens[1].lower()
            if scope in definitions: raise ValueError('duplicate subcircuit '+scope)
            definitions[scope]=dict(header=s,body=[])
        elif kind=='.ends': scope='top'
        elif kind=='.model': models.append(dict(scope=scope,file=path.name,**row))
        elif kind.startswith('.'):
            if scope!='top': definitions[scope]['body'].append(row)
        else:
            rec=dict(scope=scope,file=path.name,device=kind[0].upper(),**row)
            records.append(rec)
            if scope!='top': definitions[scope]['body'].append(rec)
out=dict(sources={str(p.relative_to(root)):v for p,v in files.items()},subcircuits=definitions,models=models,devices=records,device_counts=dict(collections.Counter(r['device'] for r in records)))
dest=root.parent/'mkii'/'circuit_inventory.json'
dest.write_text(json.dumps(out,indent=2))
print('DEVICE_COUNTS',out['device_counts'])
print('SUBCIRCUITS',list(definitions))
print('MODEL_DEFINITIONS',len(models))
print('SOURCE_HASHES',{p:v['sha256'] for p,v in out['sources'].items()})
