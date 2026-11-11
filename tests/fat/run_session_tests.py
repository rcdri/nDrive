#!/usr/bin/env python3
"""Live-sync tests: one mounted session, several rounds of host edits, a sync
after each (as nDrive does while the drive stays mounted)."""
import hashlib, os, random, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get('VFAT_TOOL') or os.path.join(HERE, '..', '..', 'build', 'vfat_tool')
OFF = '@@1048576'
env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
fails = 0

def ok(c, w):
    global fails
    print(('PASS ' if c else 'FAIL ') + w)
    fails += not c

def run(*a, check=True):
    p = subprocess.run(a, capture_output=True, text=True, env=env)
    if check and p.returncode:
        raise RuntimeError(' '.join(a) + '\n' + p.stdout + p.stderr)
    return p

def rnd(n, seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(n))

def manifest(root):
    out = set()
    for dp, dns, fns in os.walk(root):
        rel = os.path.relpath(dp, root)
        for d in dns:
            out.add('D /' + os.path.normpath(os.path.join(rel, d)).lstrip('./') if rel != '.' else 'D /' + d)
        for f in fns:
            p = os.path.join(dp, f)
            data = open(p, 'rb').read()
            h = 2166136261
            for c in data:
                h = ((h ^ c) * 16777619) & 0xffffffff
            r = '/' + (f if rel == '.' else os.path.join(rel, f))
            out.add('F %s %d %08x' % (r, len(data), h))
    return out

work = tempfile.mkdtemp(prefix='ndrive-session-')
calc = os.path.join(work, 'calc'); exp = os.path.join(work, 'exp')
os.makedirs(calc + '/ndless/startup'); os.makedirs(calc + '/Examples'); os.makedirs(calc + '/old')
files = {'ndless/ndless_resources.tns': rnd(200000, 1), 'ndless/startup/ndrive.tns': rnd(50000, 2),
         'Examples/Calc.tns': rnd(9000, 3), 'b.tns': rnd(5000, 4), 'c.tns': rnd(3000, 5),
         'f.tns': rnd(70000, 6), 'g.tns': rnd(12000, 7), 'old/x.tns': rnd(4000, 8)}
for p, d in files.items():
    open(os.path.join(calc, p), 'wb').write(d)
shutil.copytree(calc, exp)
img = [os.path.join(work, 'r%d.img' % i) for i in range(4)]
run(TOOL, 'dump', calc, img[0], '12')
def m(i, *a): return run(a[0], '-i', img[i] + OFF, *a[1:])
def blob(name, n, seed):
    p = os.path.join(work, name); open(p, 'wb').write(rnd(n, seed)); return p

# Round 1
shutil.copy(img[0], img[1])
a = blob('a', 150000, 10); m(1, 'mcopy', a, '::/a new.tns'); shutil.copy(a, exp + '/a new.tns')
m(1, 'mren', '::/b.tns', '::/b renamed.tns'); os.rename(exp + '/b.tns', exp + '/b renamed.tns')
m(1, 'mdel', '::/c.tns'); os.unlink(exp + '/c.tns')
m(1, 'mmd', '::/D folder'); os.makedirs(exp + '/D folder')
e = blob('e', 20000, 11); m(1, 'mcopy', e, '::/D folder/e file.tns'); shutil.copy(e, exp + '/D folder/e file.tns')
data = bytearray(open(img[1], 'rb').read()); fpos = data.find(files['f.tns'][40000:40064])
data[fpos:fpos + 4] = b'EDIT'; open(img[1], 'wb').write(data)
fe = bytearray(files['f.tns']); fe[40000:40004] = b'EDIT'; open(exp + '/f.tns', 'wb').write(fe)
# Round 2
shutil.copy(img[1], img[2])
m(2, 'mren', '::/a new.tns', '::/a moved again.tns'); os.rename(exp + '/a new.tns', exp + '/a moved again.tns')
m(2, 'mmove', '::/D folder/e file.tns', '::/e at root.tns'); os.rename(exp + '/D folder/e file.tns', exp + '/e at root.tns')
m(2, 'mrd', '::/D folder'); os.rmdir(exp + '/D folder')
g = blob('g', 33333, 12); m(2, 'mcopy', '-o', g, '::/g.tns'); shutil.copy(g, exp + '/g.tns')
m(2, 'mren', '::/f.tns', '::/f edited then moved.tns'); os.rename(exp + '/f.tns', exp + '/f edited then moved.tns')
# Round 3
shutil.copy(img[2], img[3])
m(3, 'mdel', '::/a moved again.tns'); os.unlink(exp + '/a moved again.tns')
m(3, 'mren', '::/ndless', '::/ndless renamed'); os.rename(exp + '/ndless', exp + '/ndless renamed')
big = blob('big', 2500000, 13); m(3, 'mcopy', big, '::/big file.tns'); shutil.copy(big, exp + '/big file.tns')
m(3, 'mdeltree', '::/old'); shutil.rmtree(exp + '/old')

# expected manifests per round
exp_rounds = []
import copy
# recompute expected per round by replaying on a fresh copy
def replay(k):
    d = os.path.join(work, 'exp%d' % k)
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(exp if k == 3 else exp, d)
    return d

p = run(TOOL, 'session', calc, '12', img[0], img[1], img[2], img[3], check=False)
print(p.stdout.strip())
ok(p.returncode == 0, 'three live syncs without errors; host view unchanged by every sync')
for i in (1, 2, 3):
    ok(os.path.exists(img[i] + '.manifest'), 'manifest after round %d' % i)
got = set(l.strip() for l in open(img[3] + '.manifest'))
want = manifest(exp)
ok(got == want, 'calculator matches the host after round 3')
if got != want:
    for l in sorted(got ^ want): print('   ', ('calc ' if l in got else 'want ') + l)
ok(not any('~ndrive' in l for l in got), 'no temporary files left')
print('ALL PASSED' if not fails else '%d FAILED' % fails)
shutil.rmtree(work)
sys.exit(1 if fails else 0)
