"""Flatten the connected retrofit SPICE deck into native MNA devices."""
import ast
from collections import Counter, defaultdict
import math
from pathlib import Path
import re
import sys

source, bias_file, dest = map(Path, sys.argv[1:])
top, subs, models, devices = [], {}, {}, []
scales = dict(meg=1e6, t=1e12, g=1e9, k=1e3, m=1e-3, u=1e-6,
              n=1e-9, p=1e-12, f=1e-15)
literal = re.compile(r"(?<![\w.])((?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)((?!e[+-]?\d)[a-z]+)(?![\w.])")

def evaluate(value, scope):
    value = literal.sub(lambda m: repr(float(m[1]) * next(
        (v for k, v in scales.items() if m[2].startswith(k)), 1)), value.strip("{}"))
    tree = ast.parse(value, mode="eval")
    allowed = (ast.Expression, ast.Load, ast.Constant, ast.Name, ast.BinOp,
               ast.UnaryOp, ast.Add, ast.Sub, ast.Mult, ast.Div, ast.USub, ast.UAdd, ast.Call)
    if any(not isinstance(node, allowed) for node in ast.walk(tree)):
        raise ValueError(f"unsupported parameter expression {value!r}")
    return float(eval(compile(tree, "<circuit-parameter>", "eval"), {"__builtins__": {}},
                      dict(log10=math.log10, max=max, **scope)))

def read(path, title=False):
    lines = []
    for line in path.read_text(encoding="latin-1").splitlines()[int(title):]:
        line = line.lower().strip().strip("\x1a")
        if not line or line.startswith("*"):
            continue
        if line.startswith("+"):
            lines[-1] += " " + line[1:].strip()
        else:
            lines.append(line)
    current = top
    for line in lines:
        args = line.split()
        directive = args[0]
        if directive == ".include":
            read(next(p for p in path.parent.rglob("*") if p.relative_to(path.parent).as_posix().lower() == args[1]))
        elif directive == ".subckt":
            split = args.index("params:") if "params:" in args else len(args)
            current = []
            subs[args[1]] = (args[2:split], args[split+1:], current)
        elif directive == ".ends":
            current = top
        elif directive != ".end":
            current.append(line)

def expand(lines, prefix="", mapping=None, parent=None):
    mapping = mapping or {}
    env = dict(parent or {})
    for line in lines:
        if line.startswith(".param "):
            for field in line.split()[1:]:
                k, v = field.split("=", 1)
                env[k] = evaluate(v, env)
        if line.startswith(".model "):
            m = re.fullmatch(r"\.model\s+(\S+)\s+(\w+)\s*\(\s*(.*?)\s*\)", line)
            if not m:
                raise ValueError(line)
            models[prefix+m[1]] = (m[2], {k:evaluate(v,env) for k,v in
                                       re.findall(r"(\w+)\s*=\s*([^ ,]+)", m[3])})
    def node(n):
        return "0" if n == "0" else mapping.get(n, prefix+n)
    for line in lines:
        orig, *args = line.split()
        kind, name = orig[0], prefix+orig
        if orig in {".model", ".param"}:
            continue
        if kind == "x":
            split = next((i for i,v in enumerate(args) if "=" in v), len(args))
            pins, defaults, body = subs[args[split-1]]
            controls = dict(env)
            for argument in [*defaults,*args[split:]]:
                k,v = argument.split("=", 1)
                controls[k] = evaluate(v, env)
            expand(body,name+".",dict(zip(pins,map(node,args[:split-1]),strict=True)),controls)
            continue
        pins = list(map(node,args[:3 if kind in "qj" else 2]))
        if kind in "dqj":
            value = prefix+args[-1] if prefix+args[-1] in models else args[-1]
        elif kind in "rcvi":
            expr = args[3] if args[2] == "dc" else args[2]
            value = 0 if name in {"vinp","vinm"} else evaluate(expr, env)
        elif kind in "egfh":
            parts = args[2:]
            if parts[0].startswith("poly("):
                count = int(parts.pop(0)[5:-1])
                ctrl = ([tuple(map(node,v.strip("()").split(","))) for v in parts[:count]]
                        if kind == "e" else [prefix+v for v in parts[:count]])
                coeff = [evaluate(arg,env) for arg in parts[count:]]
                if len(coeff) != count+1:
                    raise ValueError(line)
                value = ctrl, coeff[0], coeff[1:]
            elif kind in "eg":
                value = [(node(parts[0]),node(parts[1]))], 0, [evaluate(parts[2],env)]
            else:
                value = [prefix+parts[0]], 0, [evaluate(parts[1],env)]
        else:
            raise ValueError(line)
        devices.append((kind,name,pins,value))

read(source/"160_retrofit.cir", True)
expand(top)
used = {ctrl for k,_,_,d in devices if k in "fh" for ctrl in d[0]}
known = {"0": (0,0)}
known_codes = dict(vinp=1,vinm=2,vth=3,vmake=4)
nodes, branches = [], []
for kind,name,pins,data in devices:
    if kind == "v" and pins[1] == "0" and name not in used:
        known[pins[0]] = (known_codes.get(name,0),data)
    elif kind in "veh":
        branches.append(name)
extra = {}
for kind,name,pins,data in devices:
    ctrls = [n for pair in data[0] for n in pair] if kind in "eg" else []
    for node in [*pins,*ctrls]:
        if node not in known and node not in nodes:
            nodes.append(node)
    if kind in "dq":
        keys = ("rc","rb","re") if kind == "q" else ("rs",)
        labels = ("collector","base","emitter") if kind == "q" else ("pos",)
        for key,node,label in zip(keys,pins,labels):
            if models[data][1].get(key):
                extra[name+"#"+label] = node
                nodes.append(name+"#"+label)

variables = nodes+branches
index = {n:i for i,n in enumerate(variables)}
known_index = {n:-i-1 for i,n in enumerate(known)}
def ix(node):
    return index[node] if node in index else known_index[node]
def spice_name(name):
    if "." not in name:
        return name
    path,local = name.rsplit(".",1)
    return f"{local[0]}.{path}.{local}"

header, values = bias_file.read_text(encoding="latin-1").split("Values:",1)
vectors = re.findall(r"(?m)^\s*\d+\s+([vi]\([^\n]+?\))\s+\w+\s*$",header)
values = [float(s) for s in values.split()[1:]]
count = int(re.search(r"No. Variables: (\d+)",header)[1])
assert len(values) >= count and len(vectors) in {count,count-1}
op = dict(zip(vectors,values[-len(vectors):]))
def bias(name):
    if name in extra:
        instance,pin = name.split("#")
        ext = extra[name]
        return op.get(f"v({spice_name(instance)}#{pin})",
                      op.get(f"v({ext})",known.get(ext,(0,0))[1]))
    if name in nodes:
        return op[f"v({name})"]
    branch = spice_name(name)
    return op.get(f"i({branch})",op.get("i(a$poly$"+branch+")",0.))

def fmt(x):
    return format(x,".17g")

cpp = ["// Generated from 160_retrofit.cir and its manufacturer includes.",
       "inline void loadDeck(Circuit& c) {", f"    c.nodes = {len(nodes)};",
       f"    c.size = {len(variables)};", f"    c.out = {ix('out_plus')};",
       f"    c.control = {ix('ec_minus')};", f"    c.level = {ix('level')};",
       "    c.bias = {" + ", ".join(fmt(bias(k)) for k in variables) + "};"]
for kind,value in known.values():
    cpp.append(f"    c.constants.push_back({{{kind}, {fmt(value)}}});")
sets = {"d":("is n rs cjo vj m tt fc",[1e-14,1,0,0,1,.5,0,.5]),
        "q":("is bf nf vaf ikf ise ne br nr var ikr isc nc rb rc re cje vje mje cjc vjc mjc tf tr fc",
             [1e-16,100,1,0,0,0,1.5,1,1,0,0,0,2,0,0,0,0,.75,.33,0,.75,.33,0,0,.5]),
        "j":("vto beta is lambda",[-2,1e-4,1e-14,0])}
types = {"d":{"d"},"q":{"npn","pnp"},"j":{"njf","pjf"}}
groups = defaultdict(list)
for kind,name,pins,data in devices:
    a,b = map(ix,pins[:2])
    def add(list_name,values):
        cpp.append("    c."+list_name+".push_back({"+values+"}); // "+name)
    if kind=="v" and name not in used and pins[1]=="0":
        continue
    if kind=="r":
        add("resistors",f"{a}, {b}, {fmt(1/data)}, {int(name in {'rctl2','rctl4'})}")
    elif kind in "ci":
        add("capacitors" if kind=="c" else "currents",f"{a}, {b}, {fmt(data)}")
    elif kind=="v":
        add("sources",f"{a}, {b}, {ix(name)}, {fmt(data)}")
    elif kind in "egfh":
        ctrls,offset,gains=data
        multiplier=1
        if kind in "eh":
            k=ix(name)
            add("sources",f"{a}, {b}, {k}, {fmt(offset)}")
            a,b,multiplier=k,ix("0"),-1
        elif offset:
            add("currents",f"{a}, {b}, {fmt(offset)}")
        for ctrl,gain in zip(ctrls,gains,strict=True):
            if gain:
                p,m=(ix(ctrl),ix("0")) if kind in "fh" else map(ix,ctrl)
                add("controlled",f"{a}, {b}, {p}, {m}, {fmt(gain*multiplier)}")
    else:
        model,md=models[data]
        keys,defaults=sets[kind]
        keys=keys.split()
        assert model in types[kind] and set(md)<=set(keys),(name,model,set(md)-set(keys))
        indices=list(map(ix,pins))
        series=(("rc","collector"),("rb","base"),("re","emitter")) if kind=="q" else (("rs","pos"),)
        for i,(key,label) in enumerate(series):
            if md.get(key):
                inner=ix(name+"#"+label)
                add("resistors",f"{indices[i]}, {inner}, {fmt(1/md[key])}, 0")
                indices[i]=inner
        sign = -1 if model in {"pnp","pjf"} else 1
        model_values=", ".join(fmt(md.get(k,d)) for k,d in zip(keys,defaults,strict=True)
                               if k not in {"rs","rc","rb","re"})
        groups[(kind,model_values,sign)].append((indices,name))
offsets=defaultdict(int)
calls=defaultdict(list)
variants={"d":("diodes","DModel"),"q":("bjts","QModel"),"j":("jfets","JModel")}
for (kind,model,sign),members in groups.items():
    container,model_type=variants[kind]
    for terminals,name in members:
        cpp.append(f"    c.{container}.push_back({{{', '.join(map(str,terminals))}}}); // {name}")
    params=f"{model_type}{{{model}}}" if kind=="d" else f"{model_type}{{{model}}}, {sign}"
    calls[kind].append(f"    stamp.template operator()<{params}>({offsets[kind]}, {len(members)});")
    offsets[kind]+=len(members)
cpp.append("}")
for kind,(container,_) in variants.items():
    cpp+=["template<class Stamp>", f"inline void stamp_{container}(Stamp&& stamp) {{", *calls[kind], "}"]
dest.parent.mkdir(parents=True,exist_ok=True)
dest.write_text("\n".join(cpp)+"\n")
print("Components:",dict(Counter(row[0] for row in devices)),
      "unknowns:",len(variables),"fixed voltages:",len(known))
