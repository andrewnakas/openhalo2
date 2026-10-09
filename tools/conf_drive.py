"""Drive a kept xboxrecomp conformance harness (xbe_harness.exe) to completion.
Functions that crash or hang it are quarantined one by one (the upstream runner
only handles crashes and waits 600 s on a hang).
usage: python conf_drive.py <workdir> [timeout_s]"""
import re, subprocess, sys
wd = sys.argv[1]; tmo = float(sys.argv[2]) if len(sys.argv) > 2 else 20
exe = wd + '/xbe_harness.exe'; xbe = 'D:/n64work/halo2/dirty/game/default.xbe'
skips = []; why = {}
for _ in range(200):
    cmd = [exe, xbe] + ([','.join('%08X' % v for v in skips)] if skips else [])
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=tmo, errors='replace')
        out, rc = r.stdout, r.returncode
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode('latin1') if isinstance(e.stdout, bytes) else (e.stdout or ''); rc = 'hang'
    started = re.findall(r'@RUN ([0-9A-Fa-f]{8})', out)
    if rc in (0, 1) or not started:
        break
    v = int(started[-1], 16)
    if v in skips:
        break
    skips.append(v); why[v] = rc
print('\n'.join(l for l in out.splitlines() if not l.startswith('@RUN ')).strip())
print('exit', rc, '| quarantined:', ', '.join('sub_%08X(%s)' % (v, why[v] if why[v] == 'hang' else 'crash') for v in skips))
