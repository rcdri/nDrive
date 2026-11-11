#!/usr/bin/env python3
"""Host tests for the virtual FAT volume (src/vfat.c) using real FAT tools.

A directory plays the calculator's /documents. The volume is dumped exactly as a
computer would read it, checked with fsck.fat and mtools, then modified with
mtools (as a computer would), and the sector differences are fed back through
vfat_write + vfat_sync. The resulting directory must match the same operations
done directly on a copy.
"""
import hashlib, os, random, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get('VFAT_TOOL') or os.path.join(HERE, '..', '..', 'build', 'vfat_tool')
OFF = '@@1048576'   # partition starts at LBA 2048
env = dict(os.environ, MTOOLS_SKIP_CHECK='1', MTOOLS_NO_VFAT='0')
failures = 0


def run(*args, check=True):
    p = subprocess.run(args, capture_output=True, text=True, env=env)
    if check and p.returncode:
        raise RuntimeError('%s failed (%d):\n%s%s' % (' '.join(args), p.returncode, p.stdout, p.stderr))
    return p


def ok(cond, what):
    global failures
    print(('PASS ' if cond else 'FAIL ') + what)
    if not cond:
        failures += 1


def rnd(n, seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(n))


def make_tree(root):
    files = {
        'themes.csv': b'name,color\nblue,1\n',
        'NspireLogs.zip': b'',
        'ndless/ndless_resources.tns': rnd(210000, 1),
        'ndless/startup/nCapture.tns': rnd(40000, 2),
        'Examples/Calculus.tns': rnd(12345, 3),
        'a/b/c/d/deep.tns': rnd(5000, 4),
        'Álgebra – notas.tns': rnd(777, 5),
        '日本語ファイル.tns': rnd(4096, 6),
        ('L' * 200) + '.tns': rnd(100, 7),
        'what?.tns': rnd(300, 8),
        'a:b.tns': rnd(301, 9),
        'trail.': rnd(302, 10),
        'Case.tns': rnd(303, 11),
        'case.tns': rnd(304, 12),
        'big/big.tns': rnd(3 * 1024 * 1024 + 17, 13),
        'exact4096.tns': rnd(4096, 14),
        'exact512.tns': rnd(512, 15),
        'swap one.tns': rnd(1000, 16),
        'swap two.tns': rnd(2000, 17),
    }
    for i in range(300):
        files['many/file%03d.tns' % i] = rnd(100 + i, 100 + i)
    for p, data in files.items():
        full = os.path.join(root, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, 'wb') as f:
            f.write(data)
    os.makedirs(os.path.join(root, 'MyLib'))
    os.makedirs(os.path.join(root, 'empty/nested'))


def tree(root):
    out = {}
    for dp, dns, fns in os.walk(root):
        rel = os.path.relpath(dp, root)
        if rel != '.':
            out[rel + '/'] = None
        for f in fns:
            p = os.path.join(dp, f)
            out[os.path.normpath(os.path.join(rel, f))] = hashlib.sha1(open(p, 'rb').read()).hexdigest()
    return out


def fsck(img, work):
    part = os.path.join(work, 'part.img')
    with open(img, 'rb') as f, open(part, 'wb') as g:
        f.seek(1048576)
        shutil.copyfileobj(f, g)
    p = run('fsck.fat', '-n', '-v', part, check=False)
    os.unlink(part)
    return p


def main():
    work = tempfile.mkdtemp(prefix='ndrive-test-')
    calc = os.path.join(work, 'calc')
    os.makedirs(calc)
    make_tree(calc)
    orig = tree(calc)

    img0 = os.path.join(work, 'disk0.img')
    out = run(TOOL, 'dump', calc, img0, '8').stdout
    print(out.strip())

    p = fsck(img0, work)
    ok(p.returncode == 0 and '16 bit entries' in p.stdout, 'fsck.fat clean on generated FAT16 volume')
    if p.returncode:
        print(p.stdout[-2000:], p.stderr[-2000:])

    # Everything readable through mtools, byte identical.
    export = os.path.join(work, 'export')
    os.makedirs(export)
    run('mcopy', '-s', '-n', '-i', img0 + OFF, '::/*', export)
    got = tree(export)
    sanitized = {'what?.tns': 'what_.tns', 'a:b.tns': 'a_b.tns', 'trail.': 'trail'}
    expect = {}
    for k, h in orig.items():
        expect[sanitized.get(k, k)] = h
    # Case duplicates: one of the two gets a ~2 suffix.
    if 'case.tns' in expect and 'case~2.tns' in got:
        expect['case~2.tns'] = expect.pop('case.tns')
    elif 'Case~2.tns' in got:
        expect['Case~2.tns'] = expect.pop('Case.tns')
    ok(got == expect, 'all files and folders exported intact through mtools (%d entries)' % len(got))
    if got != expect:
        for k in sorted(set(got) | set(expect)):
            if got.get(k, 'missing') != expect.get(k, 'missing'):
                print('   ', k, got.get(k, 'missing'), expect.get(k, 'missing'))

    label = run('mlabel', '-s', '-i', img0 + OFF, '::').stdout
    ok('NSPIRE' in label, 'volume label NSPIRE')

    # ---------------------------------------------------------------- edits
    img1 = os.path.join(work, 'disk1.img')
    shutil.copy(img0, img1)
    expected = os.path.join(work, 'expected')
    shutil.copytree(calc, expected, symlinks=False)
    m = lambda *a: run(*a[:1], '-i', img1 + OFF, *a[1:])

    new_root = os.path.join(work, 'newfile.tns')
    open(new_root, 'wb').write(rnd(500000, 50))
    m('mcopy', new_root, '::/newfile.tns')
    shutil.copy(new_root, os.path.join(expected, 'newfile.tns'))

    m('mcopy', new_root, '::/Examples/copy of new.tns')
    shutil.copy(new_root, os.path.join(expected, 'Examples/copy of new.tns'))

    m('mdel', '::/themes.csv')
    os.unlink(os.path.join(expected, 'themes.csv'))

    m('mren', '::/Examples/Calculus.tns', '::/Examples/Calculus 2.tns')
    os.rename(os.path.join(expected, 'Examples/Calculus.tns'), os.path.join(expected, 'Examples/Calculus 2.tns'))

    m('mmove', '::/a/b/c/d/deep.tns', '::/MyLib/deep moved.tns')
    os.rename(os.path.join(expected, 'a/b/c/d/deep.tns'), os.path.join(expected, 'MyLib/deep moved.tns'))

    m('mmd', '::/New Folder')
    os.makedirs(os.path.join(expected, 'New Folder'))
    m('mcopy', new_root, '::/New Folder/inside.tns')
    shutil.copy(new_root, os.path.join(expected, 'New Folder/inside.tns'))

    m('mdeltree', '::/empty')
    shutil.rmtree(os.path.join(expected, 'empty'))

    # Overwrite in place with different content.
    repl = os.path.join(work, 'repl.tns')
    open(repl, 'wb').write(rnd(212000, 51))
    m('mcopy', '-o', repl, '::/ndless/ndless_resources.tns')
    shutil.copy(repl, os.path.join(expected, 'ndless/ndless_resources.tns'))

    # Rename a folder that contains files.
    m('mren', '::/ndless/startup', '::/ndless/autostart')
    os.rename(os.path.join(expected, 'ndless/startup'), os.path.join(expected, 'ndless/autostart'))

    # Swap two names through a third.
    m('mren', '::/swap one.tns', '::/swap tmp.tns')
    m('mren', '::/swap two.tns', '::/swap one.tns')
    m('mren', '::/swap tmp.tns', '::/swap two.tns')
    a, b = os.path.join(expected, 'swap one.tns'), os.path.join(expected, 'swap two.tns')
    os.rename(a, a + '.x'); os.rename(b, a); os.rename(a + '.x', b)

    # A file replaced by a folder of the same name.
    m('mdel', '::/NspireLogs.zip')
    m('mmd', '::/NspireLogs.zip')
    os.unlink(os.path.join(expected, 'NspireLogs.zip'))
    os.makedirs(os.path.join(expected, 'NspireLogs.zip'))

    # macOS-style junk must not reach the calculator.
    junk = os.path.join(work, '._junk')
    open(junk, 'wb').write(b'x' * 4096)
    m('mcopy', junk, '::/._newfile.tns')
    m('mmd', '::/.fseventsd')
    m('mcopy', junk, '::/.fseventsd/fseventsd-uuid')

    # Empty new file
    emptyf = os.path.join(work, 'empty.tns')
    open(emptyf, 'wb').close()
    m('mcopy', emptyf, '::/many/zero.tns')
    open(os.path.join(expected, 'many/zero.tns'), 'wb').close()

    p = fsck(img1, work)
    ok(p.returncode == 0, 'fsck.fat clean after host edits')

    out = run(TOOL, 'apply', calc, img0, img1, '8', check=False)
    print(out.stdout.strip())
    ok(out.returncode == 0, 'sync reported no errors')
    a, b = tree(calc), tree(expected)
    ok(a == b, 'calculator tree matches expected after sync')
    if a != b:
        for k in sorted(set(a) | set(b)):
            if a.get(k, 'missing') != b.get(k, 'missing'):
                print('   ', repr(k), a.get(k, 'missing'), b.get(k, 'missing'))
    ok(not any('~ndrive' in k for k in a), 'no temporary files left')

    # ------------------------------------------------- unchanged volume no-op
    img2 = os.path.join(work, 'disk2.img')
    run(TOOL, 'dump', calc, img2, '8')
    before = tree(calc)
    out = run(TOOL, 'apply', calc, img2, img2, '8', check=False)
    ok(out.returncode == 0 and tree(calc) == before, 'no writes -> no changes')

    # Host only reads/touches metadata (timestamps): nothing rewritten.
    img3 = os.path.join(work, 'disk3.img')
    shutil.copy(img2, img3)
    run('mattrib', '-i', img3 + OFF, '+r', '::/many/file001.tns')
    out = run(TOOL, 'apply', calc, img2, img3, '8', check=False)
    ok('written 0 moved 0 deleted 0' in out.stdout and tree(calc) == before, 'attribute change only -> nothing rewritten')

    # In-place edit of a few bytes inside an existing file (no FAT change).
    img_a = os.path.join(work, 'diskA.img')
    run(TOOL, 'dump', calc, img_a, '8')
    data = bytearray(open(img_a, 'rb').read())
    target = os.path.join(calc, 'big/big.tns')
    content = open(target, 'rb').read()
    pos = data.find(content[2000000:2000064])
    ok(pos > 0, 'located file data in image')
    patched = bytearray(content)
    patched[2000000:2000005] = b'HELLO'
    data[pos:pos + 5] = b'HELLO'
    img_b = os.path.join(work, 'diskB.img')
    open(img_b, 'wb').write(data)
    before = tree(calc)
    out = run(TOOL, 'apply', calc, img_a, img_b, '8', check=False)
    ok(out.returncode == 0 and open(target, 'rb').read() == bytes(patched), 'in-place byte edit rewrites only that file correctly')
    after = tree(calc)
    before.pop('big/big.tns'); after.pop('big/big.tns')
    ok(before == after, 'other files untouched by in-place edit')
    before = tree(calc)

    # ---------------------------------------------- mass delete needs confirm
    img4 = os.path.join(work, 'disk4.img')
    run(TOOL, 'dump', calc, img4, '8')
    img5 = os.path.join(work, 'disk5.img')
    shutil.copy(img4, img5)
    run('mdeltree', '-i', img5 + OFF, '::/many')
    out = run(TOOL, 'apply', calc, img4, img5, '8', '--no', check=False)
    ok('confirm deletes' in out.stdout and tree(calc) == before, 'mass delete declined -> calculator untouched')
    out = run(TOOL, 'apply', calc, img4, img5, '8', check=False)
    after = tree(calc)
    ok(out.returncode == 0 and not any(k.startswith('many') for k in after), 'mass delete confirmed -> applied')

    # -------------------------------------------- host formats the volume
    img6 = os.path.join(work, 'disk6.img')
    run(TOOL, 'dump', calc, img6, '8')
    img7 = os.path.join(work, 'disk7.img')
    shutil.copy(img6, img7)
    run('mformat', '-i', img7 + OFF, '-v', 'WIPED', '::', check=False)
    before = tree(calc)
    out = run(TOOL, 'apply', calc, img6, img7, '8', '--no', check=False)
    ok(tree(calc) == before, 'format from the computer is not applied without confirmation')

    # ------------------------------------------------------- free space
    img8 = os.path.join(work, 'disk8.img')
    out = run(TOOL, 'dump', calc, img8, '2').stdout
    free = run('minfo', '-i', img8 + OFF, '::', check=False)
    big = os.path.join(work, 'big.bin')
    open(big, 'wb').write(rnd(3 * 1024 * 1024, 99))
    shutil.copy(img8, img8 + '.orig')
    p = run('mcopy', '-i', img8 + OFF, big, '::/big.bin', check=False)
    if os.environ.get('VFAT_FREE_MB'):
        # Scratch file: the free space is what was asked for, however little RAM there is.
        run(TOOL, 'apply', calc, img8 + '.orig', img8, '2', check=False)
        ok(p.returncode == 0 and os.path.exists(calc + '/big.bin') and
           open(calc + '/big.bin', 'rb').read() == open(big, 'rb').read(),
           'with a scratch file a 3 MB copy fits and is saved intact (256 KB of RAM)')
    else:
        ok(p.returncode != 0 or 'Disk full' in (p.stdout + p.stderr), 'free space limited by RAM budget (3 MB copy refused at 2 MB)')

    shutil.rmtree(work)
    print('\n%s' % ('ALL PASSED' if not failures else '%d FAILED' % failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
