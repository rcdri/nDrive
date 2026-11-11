#!/usr/bin/env python3
"""End-to-end test of nDrive in the Firebird emulator (composite: TI link + MTP).

The emulated computer keeps the TI link (Firebird's usblink, as TI software or
n-link would) and at the same time speaks MTP on EP2, as a file manager does.

usage: e2e_mtp.py FBCTL_DIR NDRIVE_TNS [WORKDIR]
"""
import hashlib, os, random, shutil, struct, subprocess, sys, tempfile, time

EMU = sys.argv[1]
TNS = os.path.abspath(sys.argv[2])
WORK = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix='ndrive-bg-')
os.makedirs(WORK, exist_ok=True)
CMDS, LOG = (os.path.join(WORK, n) for n in ('cmds.txt', 'log.txt'))
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



sha = lambda p: hashlib.sha1(open(p, 'rb').read()).hexdigest()

# ------------------------------------------------------------ MTP host side
STORAGE = 0x00010001
ALL = 0xffffffff
tid = 0


def bulk_in():
    p = os.path.join(WORK, 'in.bin')
    if os.path.exists(p):
        os.unlink(p)
    send('msc in ' + p)
    sync()
    return open(p, 'rb').read() if os.path.exists(p) else b''


def mtp(op, params=(), data=None, data_in=False):
    """One MTP operation. Returns (response code, response params, data)."""
    global tid
    tid += 1
    c = os.path.join(WORK, 'cmd.bin')
    open(c, 'wb').write(struct.pack('<IHHI', 12 + 4 * len(params), 1, op, tid) +
                        b''.join(struct.pack('<I', x) for x in params))
    send('msc out ' + c)
    if data is not None:
        d = os.path.join(WORK, 'data.bin')
        open(d, 'wb').write(struct.pack('<IHHI', 12 + len(data), 2, op, tid) + data)
        send('msc out ' + d)
    r = bulk_in()
    payload = None
    if data_in and len(r) >= 12 and struct.unpack_from('<H', r, 4)[0] == 2:
        if struct.unpack_from('<I', r)[0] != len(r):
            return 0, [], r[12:]            # container length and transfer disagree
        payload = r[12:]
        r = bulk_in()
    if len(r) < 12 or struct.unpack_from('<H', r, 4)[0] != 3 or struct.unpack_from('<I', r, 8)[0] != tid:
        return 0, [], payload
    n = struct.unpack_from('<I', r)[0]
    return struct.unpack_from('<H', r, 6)[0], list(struct.unpack_from('<%dI' % ((n - 12) // 4), r, 12)), payload


def pstr(s):
    u = s.encode('utf-16-le') + b'\0\0'
    return bytes([len(u) // 2]) + u


def unpstr(d, off):
    n = d[off]
    return d[off + 1:off + 1 + 2 * n].decode('utf-16-le').rstrip('\0'), off + 1 + 2 * n


def listdir(parent):
    """{name: (handle, size, is_folder)} for a folder handle (ALL = root)."""
    rc, _, d = mtp(0x1007, (STORAGE, 0, parent), data_in=True)
    out = {}
    if rc != 0x2001:
        return out
    for h in struct.unpack_from('<%dI' % struct.unpack_from('<I', d)[0], d, 4):
        rc, _, info = mtp(0x1008, (h,), data_in=True)
        name, _ = unpstr(info, 52)
        out[name] = (h, struct.unpack_from('<I', info, 8)[0], struct.unpack_from('<H', info, 4)[0] == 0x3001)
    return out


def object_info(name, size, folder=False):
    return (struct.pack('<IHHIHIIIIIIIHII', STORAGE, 0x3001 if folder else 0x3000, 0, size, 0, 0, 0, 0, 0, 0, 0,
                        0, 1 if folder else 0, 0, 0) + pstr(name) + b'\0\0\0')


def upload(parent, name, content):
    rc, p, _ = mtp(0x100c, (STORAGE, parent), data=object_info(name, len(content)))
    if rc != 0x2001:
        return 0
    rc, _, _ = mtp(0x100d, (), data=content)
    return p[2] if rc == 0x2001 else 0


def download(handle):
    rc, _, d = mtp(0x1009, (handle,), data_in=True)
    return d if rc == 0x2001 else None


open(CMDS, 'w').close()
logf = open(LOG, 'wb')
proc = subprocess.Popen([os.path.join(EMU, 'fbctl2'), '--snapshot', 'calc/ndless_inst.snap', '--cmds', CMDS],
                        cwd=EMU, stdout=logf, stderr=subprocess.STDOUT)
try:
    ref = os.path.join(WORK, 'ref'); os.makedirs(ref, exist_ok=True)
    got = os.path.join(WORK, 'got'); os.makedirs(got, exist_ok=True)
    send('quiet 1', 'wait 1500', 'get /themes.csv %s/themes.csv' % ref,
         # nDrive measures its timer against the real-time clock when it starts:
         # give the emulator a real-time clock that follows emulated time for that.
         'put %s /ndrive.tns' % TNS, 'wait 1000', 'rtc 1', 'key enter', 'wait 6000', 'rtc 0')
    sync()
    shot('0_installed')
    # Firebird's TI link drops a request sent in certain short windows of
    # emulated time (with or without nDrive); a rerun shifts the timing.
    send('key enter', 'wait %d' % (1000 + 1700 * int(os.environ.get('NDRIVE_E2E_ATTEMPT', '1'))))
    sync()

    tail = since(lambda: send('msc attach3'))
    ok('config st=0 len=62' in tail and '090401000306010110' in tail and '0705830310000a' in tail,
       'composite descriptor: TI link interface + MTP interface (3 endpoints), 62 bytes')
    ok('string16 st=0 len=8 08034d0054005000' in tail, 'interface is named "MTP" (how libmtp recognises it)')
    ok('string1 st=0' in tail and '54004900' in tail, 'the OS still answers its own strings')
    tail = since(lambda: send('ls /'))
    ok('documents' in tail.lower() or '[ls end]' in tail, 'TI link works while MTP is connected')

    rc, _, _ = mtp(0x1002, (1,))
    ok(rc == 0x2001, 'MTP session opens')
    rc, _, d = mtp(0x1001, data_in=True)
    ok(rc == 0x2001 and 'TI-Nspire CX II'.encode('utf-16-le') in d, 'device info')
    rc, _, d = mtp(0x1004, data_in=True)
    ok(rc == 0x2001 and d == struct.pack('<II', 1, STORAGE), 'one storage')
    root = listdir(ALL)
    print('   root:', sorted(root))
    ok('documents' in root and root['documents'][2] and len(root) > 1, 'the root of the calculator is shown, not just /documents')
    docs = root['documents'][0]
    dl = listdir(docs)
    ok('themes.csv' in dl and dl['themes.csv'][1] == 191 and 'Examples' in dl and dl['Examples'][2], 'My Documents listed')
    ok(download(dl['themes.csv'][0]) == open(ref + '/themes.csv', 'rb').read(), 'download == same file over the TI link')
    shot('1_idle')

    def storage():
        rc, _, d = mtp(0x1005, (STORAGE,), data_in=True)
        return struct.unpack_from('<QQ', d, 6) if rc == 0x2001 else (0, 0)
    total, free0 = storage()
    print('   storage: %.1f MB, %.1f MB free' % (total / 1e6, free0 / 1e6))
    ok(16e6 < total < 4e9 and 0 < free0 < total, 'storage size and free space come from the calculator')

    # --- NspireLogs.zip: an empty placeholder on the calculator, which the OS
    # fills only for the TI link. Over MTP nDrive builds it from the log files.
    import io, zipfile
    z = download(dl['NspireLogs.zip'][0]) if 'NspireLogs.zip' in dl else None
    open(os.path.join(WORK, 'NspireLogs.zip'), 'wb').write(z or b'')
    try:
        zf = zipfile.ZipFile(io.BytesIO(z))
        names = zf.namelist()
        print('   NspireLogs.zip: %d bytes, %s' % (len(z), names))
        ok(dl['NspireLogs.zip'][1] == len(z) > 0 and zf.testzip() is None and len(names) >= 3 and
           sum(i.file_size for i in zf.infolist()) > 10000, 'NspireLogs.zip downloads as a valid archive of the log files')
    except Exception as e:
        ok(False, 'NspireLogs.zip is a valid archive (%r)' % (e,))

    # --- files outside My Documents can be read and written too
    logs = listdir(root['logs'][0])
    name = next((n for n, v in sorted(logs.items()) if not v[2] and 0 < v[1] < 100000 and n != 'NspireLogs.zip'), None)
    data = download(logs[name][0]) if name else None
    ok(bool(data) and len(data) == logs[name][1] and any(data), 'read a file outside My Documents (/logs/%s)' % name)
    h_tmp = upload(root['tmp'][0], 'ndrive-test.bin', b'outside' * 100)
    ok(h_tmp != 0 and download(h_tmp) == b'outside' * 100 and mtp(0x100b, (h_tmp,))[0] == 0x2001,
       'write, read back and delete a file in /tmp')

    # --- upload into My Documents, with the TI link connected
    r = random.Random(3)
    big = bytes(r.getrandbits(8) for _ in range(150000))
    h_big = upload(docs, 'drivefile.tns', big)
    ok(h_big != 0, 'upload 150 kB')
    send('wait 3000')
    sync()
    shot('2_received')
    rc, _, d = mtp(0x1005, (STORAGE,), data_in=True)
    ok(rc == 0x2001 and 'Calculator'.encode('utf-16-le') in d, 'MTP keeps answering while "Document Received" is open')
    second = bytes(r.getrandbits(8) for _ in range(20000))
    ok(upload(docs, 'second.tns', second) != 0, 'another upload while the box is open')
    tail = since(lambda: send('ls /'))
    ok('drivefile.tns 150000' in tail and 'second.tns 20000' in tail, 'both files are on the calculator (seen over the TI link)')
    send('get /drivefile.tns %s/drivefile.tns' % got)
    sync()
    ok(open(got + '/drivefile.tns', 'rb').read() == big, 'contents intact')
    used = free0 - storage()[1]
    ok(170000 <= used <= 400000, 'free space went down by what was uploaded (%d bytes for 170000)' % used)
    send('key esc', 'wait 2500')
    sync()
    shot('3_second_box')
    send('key esc', 'wait 1500')
    sync()
    shot('3_after_dialog')

    # --- transfers that end exactly on a packet boundary need a zero-length packet
    good = True
    for size in (52, 116, 500):                 # 12-byte header + size = 64, 128, 512
        content = bytes(r.getrandbits(8) for _ in range(size))
        h = upload(docs, 'edge%d.tns' % size, content)
        good &= h != 0 and download(h) == content
    ok(good, 'downloads ending exactly on a packet boundary (zero-length packet)')
    ok(download(dl['themes.csv'][0]) == open(ref + '/themes.csv', 'rb').read(), 'still in step afterwards')
    ok(download(h_big) == big, 'download the 150 kB file back')

    # --- folders, rename, move, delete
    rc, p, _ = mtp(0x100c, (STORAGE, docs), data=object_info('FromPC', 0, folder=True))
    folder = p[2] if rc == 0x2001 else 0
    note = ('from the computer\n' * 300).encode()
    h_note = upload(folder, 'note.tns', note)
    tail = since(lambda: send('ls /FromPC'))
    ok(folder and h_note and 'note.tns 5400' in tail, 'new folder and a file in it')
    rc, _, _ = mtp(0x9804, (h_note, 0xdc07), data=pstr('renamed.tns'))
    tail = since(lambda: send('ls /FromPC'))
    ok(rc == 0x2001 and 'renamed.tns 5400' in tail and 'note.tns' not in tail.replace('renamed.tns', ''), 'rename')
    rc, _, _ = mtp(0x1019, (h_note, STORAGE, docs))
    tail = since(lambda: send('ls /'))
    ok(rc == 0x2001 and 'renamed.tns 5400' in tail, 'move to another folder')
    rc, _, _ = mtp(0x100b, (h_note,))
    rc2, _, _ = mtp(0x100b, (folder,))
    rc3, _, _ = mtp(0x100b, (dl['Examples'][0],))
    tail = since(lambda: send('ls /'))
    ok(rc == rc2 == rc3 == 0x2001 and 'renamed.tns' not in tail and 'FromPC' not in tail and 'Examples' not in tail,
       'delete a file, an empty folder and a folder with contents')

    # --- a file sent over the TI link shows up for the computer
    linkf = os.path.join(WORK, 'fromlink.tns')
    open(linkf, 'wb').write(bytes(r.getrandbits(8) for _ in range(30000)))
    send('put %s /fromlink.tns' % linkf, 'wait 2000', 'key esc', 'wait 1000')
    sync()
    dl = listdir(docs)
    ok('fromlink.tns' in dl and dl['fromlink.tns'][1] == 30000 and
       download(dl['fromlink.tns'][0]) == open(linkf, 'rb').read(), 'file sent over the TI link is there on refresh')

    # --- errors leave the session usable
    rc, _, _ = mtp(0x1009, (999999,), data_in=True)
    ok(rc == 0x2009, 'unknown object refused')
    rc, _, _ = mtp(0x9999)
    ok(rc == 0x2005, 'unknown operation refused')
    ok(download(dl['themes.csv'][0]) == open(ref + '/themes.csv', 'rb').read(), 'session fine after errors')

    # --- cable pulled (nDrive goes idle) and plugged back in
    # (By now the emulated calculator has been idle for many minutes: like a
    # stock one it stays awake only while the cable is in and goes to standby
    # when unplugged. A real cable wakes it again; here the ON key does.)
    tail = since(lambda: send('msc unplug', 'wait 5000', 'key on', 'wait 3000', 'msc replug2 mtp'))
    rc, _, _ = mtp(0x1002, (1,))
    rc2, _, d = mtp(0x1004, data_in=True)
    ok('09023e0002' in tail and rc == 0x2001 and rc2 == 0x2001, 'MTP comes back after the cable is unplugged and plugged in again')
    shot('4_end')

    # --- options (ctrl+U, Options): show only My Documents
    def chord():
        send('hold ctrl', 'wait 200', 'key u', 'wait 200', 'release ctrl', 'wait 1500')
    send('key esc', 'wait 800')
    chord()
    sync()
    shot('5_status')
    send('key right', 'wait 300', 'key enter', 'wait 1200')         # Options
    sync()
    shot('5_options_mode')
    send('key esc', 'wait 1200')                                    # mode: no change
    sync()
    shot('5_options_folder')
    send('key right', 'wait 300', 'key enter', 'wait 2500')         # My Documents
    sync()
    rc, _, _ = mtp(0x1002, (1,))
    root = listdir(ALL)
    rc2, _, d = mtp(0x1005, (STORAGE,), data_in=True)
    ok(rc == 0x2001 and 'themes.csv' in root and 'phoenix' not in root and 'documents' not in root and
       'My Documents'.encode('utf-16-le') in d, 'option "My Documents": the computer now sees only that folder')
    ok(upload(ALL, 'indocs.tns', b'x' * 3000) != 0, 'upload to the top folder')
    send('wait 2500')
    sync()
    shot('5_docs_received')
    tail = since(lambda: send('key esc', 'wait 800', 'ls /'))
    ok('indocs.tns 3000' in tail, 'it lands in My Documents')

    # --- options: USB drive instead of MTP
    chord()
    send('key right', 'wait 300', 'key enter', 'wait 1200',         # Options
         'key right', 'wait 300', 'key enter', 'wait 1500',         # USB drive
         'key esc', 'wait 4000')                                    # folder: no change
    sync()
    shot('6_drive')
    tail = since(lambda: send('msc attach2'))
    ok('len=58' in tail and '0904010002080650' in tail and '[msc inquiry status=0' in tail and '[msc sectors ' in tail,
       'option "USB drive": the calculator is now a mass-storage device and answers SCSI')
    sectors = int(tail.split('[msc sectors ')[1].split(']')[0]) if '[msc sectors ' in tail else 0
    print('   drive: %.1f MB' % (sectors * 512 / 1e6))
    ok(sectors * 512 > 30e6, 'the drive offers real space (scratch file), not just the RAM buffer')
    send('get /ndless/ndrive.cfg.tns %s/cfg.txt' % got)
    sync()
    cfg = open(got + '/cfg.txt').read() if os.path.exists(got + '/cfg.txt') else ''
    ok('mode=drive' in cfg and 'root=documents' in cfg, 'the choices are saved for the next start')
    log = open(LOG, 'rb').read().decode('latin1')
    ok('data abort' not in log and 'BOOT: loading' not in log, 'no crash or reboot')
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
