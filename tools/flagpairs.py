"""flagpairs.py <insn_diff workdir>: which failing (producer form, condition)
pairs from insn_diff's result actually occur in the game's code, and where."""
import json, re, sys, collections
sys.path.insert(0, "D:/n64work/openhalo2/tools")
import insn_diff as I
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
wd = sys.argv[1]
r = json.load(open(f"{wd}/result.json")); m = json.load(open(f"{wd}/meta.json"))
T = {t["va"]: t for t in m["tests"]}
bad = collections.Counter()
for va, lst in r["fails"].items():
    t = T[int(va, 16)]
    real = [w for vi, cw, why in lst for w in why if not w.startswith("ebp ")]
    if t["variant"] != "plain" and real:
        bad[(t["form"], t["variant"])] += len(real)
CC = {"o": "jo", "no": "jo", "b": "jb", "c": "jb", "nae": "jb", "ae": "jb", "nb": "jb", "nc": "jb",
      "e": "je", "z": "je", "ne": "je", "nz": "je", "be": "jbe", "na": "jbe", "a": "jbe", "nbe": "jbe",
      "s": "js", "ns": "js", "p": "jp", "pe": "jp", "np": "jp", "po": "jp", "l": "jl", "nge": "jl",
      "ge": "jl", "nl": "jl", "le": "jle", "ng": "jle", "g": "jle", "nle": "jle"}
sys.path.insert(0, I.XR)
from tools.conformance import xbe_run
data, sections, _ = xbe_run.load(I.XBE)
starts = sorted({int(h, 16) for h in re.findall(r"\bsub_([0-9A-Fa-f]{8})\s*\(void\)", open(I.FUNCS_H).read())})
cs = Cs(CS_ARCH_X86, CS_MODE_32); cs.detail = True
text = [s for s in sections if s.is_code]
hits = collections.defaultdict(list)
for i, va in enumerate(starts):
    sec = next((s for s in text if s.va <= va < s.va + s.raw_size), None)
    if not sec: continue
    end = min(starts[i + 1] if i + 1 < len(starts) else va + 0x4000, va + 0x4000, sec.va + sec.raw_size)
    off = sec.raw_off + (va - sec.va)
    window = []
    for insn in cs.disasm(data[off:off + (end - va)], va):
        mn = insn.mnemonic
        cond = None
        if mn.startswith("j") and mn != "jmp" and not mn.startswith("jecxz") and not mn.startswith("jcxz"):
            cond = CC.get(mn[1:])
        elif mn.startswith("set"):
            cond = CC.get(mn[3:])
        elif mn.startswith("cmov"):
            cond = CC.get(mn[4:])
        elif mn in ("adc", "sbb", "rcl", "rcr"):
            cond = "jb"
        if cond:
            # nearest earlier instruction that writes the flags
            for p in reversed(window):
                fl = I._flag_state(p)
                if any(v != "none" for v in fl.values()):
                    k = (I.form_key(p), cond)
                    if k in bad:
                        hits[k].append((p.address, f"{p.mnemonic} {p.op_str}", insn.address, f"{mn} {insn.op_str}"))
                    break
        if insn.group(1) or insn.group(2) or insn.group(3):     # jump/call/ret end the window
            window = []
        else:
            window = (window + [insn])[-6:]
tot = 0
for k in sorted(hits, key=lambda k: -len(hits[k])):
    v = hits[k]; tot += len(v)
    print(f"{len(v):5d}  [{k[0]}] -> {k[1]}   e.g. {v[0][0]:08X} {v[0][1]} ; {v[0][2]:08X} {v[0][3]}")
print(f"total {tot} sites in {len(hits)} failing pairs")
json.dump({f"{a}|{b}": v for (a, b), v in hits.items()}, open(f"{wd}/flagpairs.json", "w"))
