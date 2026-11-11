#!/usr/bin/env python3
"""End-to-end test of nDrive's USB drive mode in the Firebird emulator (TI link + mass storage).

The emulated computer keeps the TI link (Firebird's usblink, as TI software or
n-link would) and at the same time uses the mass storage interface on EP2.

usage: e2e_fat.py FBCTL_DIR NDRIVE_TNS [WORKDIR]
"""
import hashlib, os, random, shutil, struct, subprocess, sys, tempfile, time

EMU = sys.argv[1]
TNS = os.path.abspath(sys.argv[2])
WORK = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix='ndrive-bg-')
os.makedirs(WORK, exist_ok=True)
CMDS, LOG, IMG = (os.path.join(WORK, n) for n in ('cmds.txt', 'log.txt', 'disk.img'))
OFF = '@@1048576'
env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
failures = 0
marker = 0


def ok(c, what):
    global failures
    print(('PASS ' if c else 'FAIL ') + what, flush=True)
    failures += not c


def send(*lines):
    with open(CMDS, 'a') as f:
        for l in lines:
            f.write(l + '\n')


def sync(timeout=600):
    global marker
    marker += 1
    tag = 'SYNC%d' % marker
    send('echo ' + tag)
    t0 = time.time()
    while time.time() - t0 < timeout:
        data = open(LOG, 'rb').read().decode('latin1')
        if '\n' + tag in data:
            return data
        if proc.poll() is not None:
            raise RuntimeError('emulator exited:\n' + data[-3000:])
        time.sleep(0.2)
    raise RuntimeError('timeout waiting for ' + tag)


def since(fn):
    before = len(open(LOG, 'rb').read())
    fn()
    return sync()[before:]


def shot(name):
    p = os.path.join(WORK, name + '.ppm')
    send('shot ' + p)
    sync()
    try:
        from PIL import Image
        Image.open(p).save(os.path.join(WORK, name + '.png'))
    except Exception:
        pass


def write_map(path, ranges):
    with open(path, 'w') as f:
        for lba, n in ranges:
            f.write('%d %d\n' % (lba, n))


def coalesce(sectors):
    out = []
    for s in sorted(sectors):
        if out and out[-1][0] + out[-1][1] == s:
            out[-1][1] += 1
        else:
            out.append([s, 1])
    return out


def read_ranges(ranges):
    m = os.path.join(WORK, 'read.map')
    write_map(m, ranges)
    tail = since(lambda: send('msc readmap %s %s' % (m, IMG)))
    return '0 failures' in tail


def host_read_volume(total):
    with open(IMG, 'wb') as f:
        f.truncate(total * 512)
    g1 = read_ranges([(0, 1), (2048, 1)])
    bs = open(IMG, 'rb').read()[2048 * 512:2049 * 512]
    spc = bs[13]
    rsvd, nfat, rootents, tot16, fatsz = struct.unpack_from('<HBHHxH', bs, 14)
    rootsecs = rootents * 32 // 512
    data0 = 2048 + rsvd + nfat * fatsz + rootsecs
    g2 = read_ranges([(2048, rsvd + nfat * fatsz + rootsecs)])
    img = open(IMG, 'rb').read()
    fat = img[(2048 + rsvd) * 512:(2048 + rsvd + fatsz) * 512]
    secs = set()
    for c in range(2, len(fat) // 2):
        v = struct.unpack_from('<H', fat, 2 * c)[0]
        if v and v != 0xfff7:
            secs.update(data0 + (c - 2) * spc + k for k in range(spc))
    g3 = read_ranges([tuple(r) for r in coalesce(secs)])
    return g1 and g2 and g3


def fsck(path):
    part = path + '.part'
    with open(path, 'rb') as f, open(part, 'wb') as g:
        f.seek(1048576)
        shutil.copyfileobj(f, g)
    p = subprocess.run(['fsck.fat', '-n', part], capture_output=True, text=True)
    os.unlink(part)
    return p.returncode == 0


def mt(*args):
    return subprocess.run([args[0], '-i', IMG + OFF] + list(args[1:]), capture_output=True, text=True, env=env)


def write_changes(orig):
    a, b = open(orig, 'rb').read(), open(IMG, 'rb').read()
    changed = [i for i in range(len(a) // 512) if a[i * 512:(i + 1) * 512] != b[i * 512:(i + 1) * 512]]
    m = os.path.join(WORK, 'write.map')
    write_map(m, coalesce(changed))
    tail = since(lambda: send('msc writemap %s %s' % (m, IMG)))
    return '0 failures' in tail, len(changed)


sha = lambda p: hashlib.sha1(open(p, 'rb').read()).hexdigest()

open(CMDS, 'w').close()
logf = open(LOG, 'wb')
proc = subprocess.Popen([os.path.join(EMU, 'fbctl2'), '--snapshot', 'calc/ndless_inst.snap', '--cmds', CMDS],
                        cwd=EMU, stdout=logf, stderr=subprocess.STDOUT)
try:
    ref = os.path.join(WORK, 'ref'); os.makedirs(ref, exist_ok=True)
    got = os.path.join(WORK, 'got'); os.makedirs(got, exist_ok=True)
    # Start as a USB drive showing My Documents (the settings file nDrive reads).
    cfg = os.path.join(WORK, 'ndrive.cfg.tns')
    open(cfg, 'w').write('notify=on\nmode=drive\nroot=documents\n')
    send('quiet 1', 'wait 1500', 'put %s /ndless/ndrive.cfg.tns' % cfg, 'wait 1000')
    send('get /themes.csv %s/themes.csv' % ref,
         # nDrive measures its timer against the real-time clock when it starts:
         # give the emulator a real-time clock that follows emulated time for that.
         'put %s /ndrive.tns' % TNS, 'wait 1000', 'rtc 1', 'key enter', 'wait 6000', 'rtc 0')
    sync()
    shot('0_installed')
    # Firebird's TI link drops a request sent in certain short windows of
    # emulated time (with or without nDrive); a rerun shifts the timing.
    send('key enter', 'wait %d' % (1000 + 1700 * int(os.environ.get('NDRIVE_E2E_ATTEMPT', '1'))))
    sync()

    tail = since(lambda: send('msc attach2'))
    ok('len=58' in tail and '0904010002080650' in tail,
       'composite descriptor: TI link interface + mass storage interface')
    ok('[msc enumerated]' in tail and '[msc inquiry status=0' in tail, 'drive answers on EP2 in the background')
    total = int(tail.split('[msc sectors ')[1].split(']')[0])
    tail = since(lambda: send('ls /'))
    ok('themes.csv 191' in tail and '[ls end]' in tail, 'TI link works while the drive is connected')
    shot('1_idle')

    ok(host_read_volume(total), 'computer reads the whole drive')
    ok(fsck(IMG), 'fsck.fat clean')
    exp = os.path.join(WORK, 'export'); shutil.rmtree(exp, ignore_errors=True); os.makedirs(exp)
    mt('mcopy', '-s', '-n', '::/*', exp)
    ok(sha(exp + '/themes.csv') == sha(ref + '/themes.csv'), 'file on drive == same file over TI link')

    # --- the computer copies files in, with the TI link connected
    shutil.copy(IMG, IMG + '.orig')
    r = random.Random(3)
    newf = os.path.join(WORK, 'drive_file.tns')
    open(newf, 'wb').write(bytes(r.getrandbits(8) for _ in range(150000)))
    note = os.path.join(WORK, 'note.tns')
    open(note, 'w').write('from the computer\n' * 300)
    mt('mcopy', newf, '::/drivefile.tns')
    mt('mmd', '::/FromPC')
    mt('mcopy', note, '::/FromPC/note.tns')
    mt('mren', '::/themes.csv', '::/themes2.csv')
    good, n = write_changes(IMG + '.orig')
    ok(good, 'computer wrote %d sectors' % n)
    send('wait 4000')
    sync()
    shot('2_received')
    log = open(LOG, 'rb').read().decode('latin1')
    # The OS "Document Received" dialog is on screen now; the drive must keep
    # answering meanwhile (nDrive serves it from inside the OS dialog loop).
    tail = since(lambda: send('msc tur'))
    ok('[msc tur status=0' in tail, 'drive keeps answering while "Document Received" is open')
    # A second copy while that box is still open must be saved too, not wait
    # for someone to close the box.
    shutil.copy(IMG, IMG + '.orig')
    second = os.path.join(WORK, 'second.tns')
    open(second, 'wb').write(bytes(r.getrandbits(8) for _ in range(20000)))
    mt('mcopy', second, '::/second.tns')
    good, n = write_changes(IMG + '.orig')
    send('wait 5000')
    sync()
    tail = since(lambda: send('ls /'))
    ok(good and 'second.tns 20000' in tail.lower(), 'a copy made while the box is open is saved without closing it')
    send('key esc', 'wait 2500')
    sync()
    shot('3_second_box')
    send('key esc', 'wait 1500')
    sync()
    shot('3_after_dialog')
    tail = since(lambda: send('ls /', 'ls /FromPC'))
    low = tail.lower()  # mtools writes 8.3 names upper case without the NT lower-case flags
    ok('drivefile.tns 150000' in low and 'themes2.csv 191' in low and ' themes.csv ' not in low,
       'saved to the calculator automatically (new file, rename)')
    ok('note.tns 5400' in tail, 'new folder and its file saved')
    send('get /drivefile.tns %s/drivefile.tns' % got, 'get /FromPC/note.tns %s/note.tns' % got)
    sync()
    ok(sha(got + '/drivefile.tns') == sha(newf) and sha(got + '/note.tns') == sha(note), 'contents intact')

    # --- a file sent over the TI link appears on the drive (new medium)
    linkf = os.path.join(WORK, 'fromlink.tns')
    open(linkf, 'wb').write(bytes(r.getrandbits(8) for _ in range(30000)))
    send('put %s /fromlink.tns' % linkf, 'wait 2000', 'key esc', 'wait 9000')
    sync()
    tail = since(lambda: send('msc tur'))
    ok('[msc tur status=1' in tail and '700006' in tail and '0000280000' in tail,
       'computer is told the medium changed (UNIT ATTENTION 28/00)')
    total = int(tail.split('[msc sectors ')[1].split(']')[0])
    ok(host_read_volume(total), 'computer re-reads the drive')
    ok(fsck(IMG), 'fsck.fat clean on the refreshed drive')
    exp2 = os.path.join(WORK, 'export2'); shutil.rmtree(exp2, ignore_errors=True); os.makedirs(exp2)
    mt('mcopy', '-s', '-n', '::/*', exp2)
    ok(os.path.exists(exp2 + '/fromlink.tns') and sha(exp2 + '/fromlink.tns') == sha(linkf),
       'file sent over the TI link is on the drive')
    ok(os.path.exists(exp2 + '/drivefile.tns') and sha(exp2 + '/drivefile.tns') == sha(newf),
       'file copied through the drive is still there')
    # --- a copy larger than the RAM buffer (at most 12 MB): the rest waits in
    # a scratch file on the calculator until it is saved
    free = mt('mdir', '::/').stdout.strip().splitlines()[-1]
    print('   drive free:', free.strip())
    free_bytes = int(''.join(c for c in free.split('bytes')[0] if c.isdigit()) or 0)
    ok(free_bytes > 30e6, 'the drive shows real free space (about half of the calculator\'s), not the RAM buffer')
    shutil.copy(IMG, IMG + '.orig')
    bigf = os.path.join(WORK, 'big14.tns')
    open(bigf, 'wb').write(random.Random(14).randbytes(14 * 1024 * 1024))
    mt('mcopy', bigf, '::/big14.tns')
    good, n = write_changes(IMG + '.orig')
    send('wait 20000')
    sync()
    send('key esc', 'wait 1500')
    tail = since(lambda: send('ls /'))
    ok(good and 'big14.tns %d' % (14 * 1024 * 1024) in tail.lower(), 'a 14 MB file is saved (%d sectors written)' % n)
    send('get /big14.tns %s/big14.tns' % got)
    sync(timeout=900)
    ok(os.path.exists(got + '/big14.tns') and sha(got + '/big14.tns') == sha(bigf), '14 MB file intact on the calculator')
    send('rm /big14.tns', 'wait 9000')
    sync()
    tail = since(lambda: send('msc tur'))
    total = int(tail.split('[msc sectors ')[1].split(']')[0]) if '[msc sectors ' in tail else total
    ok(host_read_volume(total), 'drive readable after that')

    # --- the computer deletes almost everything: the calculator asks first
    shutil.copy(IMG, IMG + '.orig')
    for d in ('Examples', 'MyLib', 'MyWidgets', 'PyLib', 'FromPC'):
        mt('mdeltree', '::/' + d)
    good, n = write_changes(IMG + '.orig')
    send('wait 4000')
    sync()
    shot('5_delete_question')
    tail = since(lambda: send('msc tur'))
    ok(good and '[msc tur status=1' in tail and '700002' in tail and '0401' in tail,
       'while the delete question waits, the computer is told "becoming ready"')
    send('key esc', 'wait 3000')
    sync()
    tail = since(lambda: send('ls /'))
    ok('Examples' in tail and 'PyLib' in tail, 'declined: nothing deleted on the calculator')
    tail = since(lambda: send('msc tur'))
    ok('700006' in tail, 'declined: computer gets the real contents back (medium changed)')
    shot('5_after_question')

    # --- cable pulled (nDrive goes idle) and plugged back in
    tail = since(lambda: send('msc unplug', 'wait 10000', 'msc replug2'))
    ok('09023a0002' in tail and '[msc tur status=0' in tail and '[msc enumerated]' in tail,
       'drive comes back after the cable is unplugged and plugged in again')
    shot('4_end')
    ok('data abort' not in open(LOG, 'rb').read().decode('latin1') and
       'BOOT: loading' not in open(LOG, 'rb').read().decode('latin1'), 'no crash or reboot')
except Exception as e:           # e.g. a file the TI link failed to fetch
    ok(False, 'test stopped: %r' % (e,))
finally:
    send('quit')
    try:
        proc.wait(30)
    except Exception:
        proc.kill()
    logf.close()

# Firebird's TI link (not nDrive: it happens without nDrive installed too)
# loses a request now and then and never retransmits; fbctl's watchdog reports
# it. The run says nothing then: start over, a few times at most.
attempt = int(os.environ.get('NDRIVE_E2E_ATTEMPT', '1'))
if failures and 'usb watchdog' in open(LOG, 'rb').read().decode('latin1') and attempt < 5:
    print('\nemulator TI link glitch, running again (attempt %d)\n' % (attempt + 1), flush=True)
    os.environ['NDRIVE_E2E_ATTEMPT'] = str(attempt + 1)
    os.execv(sys.executable, [sys.executable] + sys.argv)

print('\nwork dir:', WORK)
print('ALL PASSED' if not failures else '%d FAILED' % failures)
sys.exit(1 if failures else 0)
