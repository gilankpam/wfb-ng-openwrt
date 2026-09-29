#!/usr/bin/env python3
"""End-to-end tests of mabur-relay through real localhost sockets.

RX is a unix SOCK_DGRAM socket (-R) fed with the bench fixtures; `iw` is
tests/iw-stub on PATH. Run from anywhere: paths resolve relative to this file.
"""
import os, shutil, socket, struct, subprocess, tempfile, time, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
RELAY = os.path.join(SRC, 'mabur-relay')

FRAME, HELLO, TUNE, STATUS = 1, 2, 3, 4

def load_fixtures():
    d = open(os.path.join(HERE, 'fixtures', 'frames.pcap'), 'rb').read()
    out, o = [], 24
    while o + 16 <= len(d):
        n = struct.unpack('<I', d[o + 8:o + 12])[0]
        out.append(d[o + 16:o + 16 + n]); o += 16 + n
    return out

FX = load_fixtures()          # 0 qos, 1 probe, 2 foreign, 3 bad-fcs

def hdr(t): return struct.pack('<HBB', 0x524D, 2, t)
def hello(): return hdr(HELLO)
def tune(tid, ch, sec): return hdr(TUNE) + struct.pack('<HBB', tid, ch, sec)

def rt_len(f): return struct.unpack('<H', f[2:4])[0]
def dot11(f): return f[rt_len(f):-4]      # fixtures carry radiotap FCS flag 0x10

def parse(msg):
    magic, ver, t = struct.unpack('<HBB', msg[:4])
    assert magic == 0x524D and ver == 2, msg[:4]
    if t == FRAME:
        seq, ch, sec, fl, mcs, r0, r1, n0, n1, tsf = struct.unpack('<IBBBBbbbbI', msg[4:20])
        return ('F', dict(seq=seq, ch=ch, sec=sec, flags=fl, mcs=mcs,
                           rssi=(r0, r1), noise=(n0, n1), tsf=tsf, body=msg[20:]))
    if t == STATUS:
        f = struct.unpack('<HBBBBBIIIIII', msg[4:35])
        keys = ['tune_id', 'state', 'ch', 'sec', 'owner', 'you_own',
                'rx', 'fwd', 'foreign', 'bad_fcs', 'your_drops', 'uptime_s']
        return ('S', dict(zip(keys, f)))
    return ('?', t)

def free_port(kind):
    s = socket.socket(socket.AF_INET, kind); s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]; s.close(); return p

def spawn_second(relay, udp_port, ws_port):
    """A second relay process sharing relay's iw stub, with its own rx socket.
    Returns the Popen; caller must .wait()/.kill() and clean up its tmpdir."""
    d = tempfile.mkdtemp(prefix='mrelay2')
    bindir = os.path.join(relay.dir, 'bin')
    env = dict(os.environ, PATH=bindir + ':' + os.environ['PATH'], IW_STUB_DIR=relay.dir)
    proc = subprocess.Popen(
        [RELAY, '-i', 'mon0', '-u', str(udp_port), '-w', str(ws_port),
         '-c', '136', '-s', '2', '-S', os.path.join(d, 'state'),
         '-R', os.path.join(d, 'rx.sock')], env=env)
    return proc, d

class Relay:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix='mrelay')
        bindir = os.path.join(self.dir, 'bin'); os.mkdir(bindir)
        shutil.copy(os.path.join(HERE, 'iw-stub'), os.path.join(bindir, 'iw'))
        os.chmod(os.path.join(bindir, 'iw'), 0o755)
        self.rx_path = os.path.join(self.dir, 'rx.sock')
        self.udp_port = free_port(socket.SOCK_DGRAM)
        self.ws_port = free_port(socket.SOCK_STREAM)
        env = dict(os.environ, PATH=bindir + ':' + os.environ['PATH'], IW_STUB_DIR=self.dir)
        self.proc = subprocess.Popen(
            [RELAY, '-i', 'mon0', '-u', str(self.udp_port), '-w', str(self.ws_port),
             '-c', '136', '-s', '2', '-S', os.path.join(self.dir, 'state'),
             '-R', self.rx_path], env=env)
        self.rx = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        for _ in range(100):
            if os.path.exists(self.rx_path): break
            time.sleep(0.02)
        self.rx.connect(self.rx_path)

    def mode(self, m): open(os.path.join(self.dir, 'mode'), 'w').write(m)
    def calls(self):
        p = os.path.join(self.dir, 'calls')
        return open(p).read().split('\n') if os.path.exists(p) else []
    def inject(self, frames):
        for f in frames: self.rx.send(f)
    def stop(self):
        self.proc.terminate(); self.proc.wait(5); shutil.rmtree(self.dir)

class Udp:
    def __init__(self, relay):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.s.bind(('127.0.0.1', 0)); self.addr = ('127.0.0.1', relay.udp_port)
    def send(self, b): self.s.sendto(b, self.addr)
    def recv_all(self, quiet=0.3):
        out = []; self.s.settimeout(quiet)
        try:
            while True: out.append(parse(self.s.recv(65536)))
        except socket.timeout: pass
        return out
    def frames(self, msgs): return [m for k, m in msgs if k == 'F']
    def statuses(self, msgs): return [m for k, m in msgs if k == 'S']

class RelayTestBase(unittest.TestCase):
    def setUp(self): self.r = Relay()
    def tearDown(self): self.r.stop()

class UdpTests(RelayTestBase):
    def test_hello_gets_status_with_startup_readback(self):
        u = Udp(self.r); u.send(hello())
        st = u.statuses(u.recv_all())
        # 1-2 STATUS: the ownership change broadcast, then the HELLO reply.
        self.assertIn(len(st), (1, 2))
        self.assertEqual((st[-1]['state'], st[-1]['ch'], st[-1]['sec']), (0, 136, 2))
        self.assertEqual((st[-1]['owner'], st[-1]['you_own']), (1, 1))

    def test_two_subscribers_get_every_forwardable_frame(self):
        a, b = Udp(self.r), Udp(self.r)
        a.send(hello()); b.send(hello()); time.sleep(0.1)
        a.recv_all(0.1); b.recv_all(0.1)
        self.r.inject(FX * 25)                       # 100 frames: 75 forwardable
        for sub in (a, b):
            fr = sub.frames(sub.recv_all())
            self.assertEqual(len(fr), 75)
            seqs = [f['seq'] for f in fr]
            self.assertEqual(seqs, list(range(seqs[0], seqs[0] + 75)))
            self.assertEqual({f['ch'] for f in fr}, {136})
            bodies = [f['body'] for f in fr[:3]]
            self.assertEqual(bodies, [dot11(FX[0]), dot11(FX[1]), dot11(FX[3])])   # foreign (2) dropped
            self.assertEqual([f['flags'] & 1 for f in fr[:3]], [0, 0, 1])

    def test_counters_in_status(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        self.r.inject(FX * 2); time.sleep(0.1)
        u.send(hello())
        st = u.statuses(u.recv_all())[-1]
        self.assertEqual((st['rx'], st['fwd'], st['foreign'], st['bad_fcs']), (8, 6, 2, 2))

    def test_malformed_frames_not_forwarded(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        self.r.inject([FX[0][:30], b'\x00' * 7, FX[0]])
        fr = u.frames(u.recv_all())
        self.assertEqual([f['body'] for f in fr], [dot11(FX[0])])

    def test_subscriber_reaped_after_2s_without_hello(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        time.sleep(2.5)
        self.r.inject(FX[:1])
        self.assertEqual(u.frames(u.recv_all()), [])

    def test_fifth_udp_subscriber_refused(self):
        subs = [Udp(self.r) for _ in range(5)]
        for s in subs[:4]: s.send(hello())
        time.sleep(0.1)
        subs[4].send(hello())
        st = subs[4].statuses(subs[4].recv_all())
        self.assertEqual([s['state'] for s in st], [3])
        self.r.inject(FX[:1])
        self.assertEqual(subs[4].frames(subs[4].recv_all()), [])

    def test_fifth_udp_tune_refused_with_its_id(self):
        subs = [Udp(self.r) for _ in range(5)]
        for s in subs[:4]: s.send(hello())
        time.sleep(0.1)
        subs[4].send(tune(42, 149, 1))
        st = subs[4].statuses(subs[4].recv_all())
        self.assertEqual([(s['state'], s['tune_id']) for s in st], [(3, 42)])
        self.assertEqual([c for c in self.r.calls() if c], [])

    def test_retune_ok_then_state_file(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        u.send(tune(5, 149, 1))
        st = u.statuses(u.recv_all(0.5))
        self.assertEqual([(s['state'], s['tune_id']) for s in st], [(1, 5), (0, 5)])
        self.assertEqual((st[-1]['ch'], st[-1]['sec']), (149, 1))
        self.assertEqual(open(os.path.join(self.r.dir, 'state')).read().strip(), '149 HT40+')
        self.r.inject(FX[:1])
        self.assertEqual(u.frames(u.recv_all())[0]['ch'], 149)

    def test_retune_failure_reports_readback(self):
        self.r.mode('fail')
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        u.send(tune(6, 149, 1))
        st = u.statuses(u.recv_all(0.5))
        self.assertEqual([s['state'] for s in st], [1, 2])
        self.assertEqual((st[-1]['ch'], st[-1]['sec'], st[-1]['tune_id']), (136, 2, 6))

    def test_retune_watchdog(self):
        self.r.mode('sleep')
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        seen = []
        deadline = time.time() + 3.0
        u.s.settimeout(0.2)
        t_sent = time.time(); u.send(tune(7, 149, 1))
        state2_at = None
        while time.time() < deadline:
            try:
                msg = u.s.recv(65536)
            except socket.timeout:
                continue
            k, m = parse(msg)
            if k != 'S':
                continue
            seen.append(m['state'])
            if m['state'] == 2 and state2_at is None:
                state2_at = time.time()
                break
        self.assertEqual(seen, [1, 2])
        self.assertIsNotNone(state2_at, "never saw state-2 STATUS")
        self.assertLess(state2_at - t_sent, 2.0)

    def test_invalid_tune_rejected_without_spawn(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        u.send(tune(8, 6, 0)); u.send(tune(9, 136, 3))
        st = u.statuses(u.recv_all())
        self.assertEqual([(s['state'], s['tune_id']) for s in st], [(2, 8), (2, 9)])
        self.assertEqual([c for c in self.r.calls() if c], [])

    def test_rapid_tunes_coalesce(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        u.send(tune(10, 36, 0)); u.send(tune(11, 40, 0)); u.send(tune(12, 44, 0))
        st = u.statuses(u.recv_all(0.8))
        self.assertEqual(st[-1]['state'], 0); self.assertEqual(st[-1]['tune_id'], 12)
        self.assertEqual(st[-1]['ch'], 44)
        self.assertNotIn(11, [s['tune_id'] for s in st if s['state'] == 1])
        self.assertNotIn('set 40 HT20', self.r.calls())

    def test_non_owner_tune_refused(self):
        a, b = Udp(self.r), Udp(self.r)
        a.send(hello()); time.sleep(0.05); b.send(hello()); a.recv_all(0.1); b.recv_all(0.1)
        b.send(tune(13, 149, 1))
        st = b.statuses(b.recv_all())
        self.assertEqual([(s['state'], s['tune_id'], s['you_own']) for s in st], [(3, 13, 0)])
        self.assertEqual([c for c in self.r.calls() if c], [])

    def test_ownership_passes_on_reap(self):
        a, b = Udp(self.r), Udp(self.r)
        a.send(hello()); time.sleep(0.05); b.send(hello()); b.recv_all(0.1)
        for _ in range(6):                          # keep b alive, let a lapse
            time.sleep(0.45); b.send(hello())
        st = b.statuses(b.recv_all())
        self.assertEqual(st[-1]['you_own'], 1)
        b.send(tune(14, 149, 1))
        self.assertEqual(b.statuses(b.recv_all(0.5))[-1]['state'], 0)

    def test_second_relay_same_ws_port_exits(self):
        # F2: a fatal ws_open_listener() must fail the process, not run with
        # WS silently disabled. Fresh UDP port so only the WS bind collides.
        proc, d = spawn_second(self.r, free_port(socket.SOCK_DGRAM), self.r.ws_port)
        try:
            rc = proc.wait(timeout=2)
            self.assertNotEqual(rc, 0)
        finally:
            if proc.poll() is None:
                proc.kill(); proc.wait(5)
            shutil.rmtree(d, ignore_errors=True)

    def test_second_relay_same_udp_port_exits(self):
        # F3: no SO_REUSEADDR on the UDP socket, so a colliding bind is fatal.
        # Fresh WS port so only the UDP bind collides.
        proc, d = spawn_second(self.r, self.r.udp_port, free_port(socket.SOCK_STREAM))
        try:
            rc = proc.wait(timeout=2)
            self.assertNotEqual(rc, 0)
        finally:
            if proc.poll() is None:
                proc.kill(); proc.wait(5)
            shutil.rmtree(d, ignore_errors=True)

class V2Tests(RelayTestBase):
    def test_v2_body_is_dot11_without_radiotap_or_fcs(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        self.r.inject([FX[0], FX[1], FX[3]])
        fr = u.frames(u.recv_all())
        self.assertEqual([f['body'] for f in fr], [dot11(FX[0]), dot11(FX[1]), dot11(FX[3])])

    def test_v2_metadata_from_radiotap(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        self.r.inject([FX[0], FX[1], FX[3]])
        q, p, b = u.frames(u.recv_all())
        self.assertEqual((q['mcs'], q['flags'] & 0x7d, q['rssi'], q['tsf']), (4, 0x10, (-128, -128), 86499716))
        self.assertEqual((p['mcs'], p['flags'] & 0x7d, p['rssi'], p['noise'], p['tsf']),
                         (0, 0x14, (-37, -44), (-95, -95), 86535458))
        self.assertEqual((b['mcs'], b['flags'] & 0x7d, b['rssi']), (0xFF, 0x05, (-37, -44)))

    def test_tiny_frame_keeps_header(self):
        u = Udp(self.r); u.send(hello()); u.recv_all(0.1)
        tiny = FX[0][:rt_len(FX[0]) + 26]           # dot11 part 26 B < 28: no FCS strip
        self.r.inject([tiny])
        fr = u.frames(u.recv_all())
        self.assertEqual(fr[0]['body'], tiny[rt_len(tiny):])

    def test_ws_batch_order(self):
        u, w = Udp(self.r), Ws(self.r)
        u.send(hello()); w.send(hello()); time.sleep(0.1); u.recv_all(0.1); w.recv_all(0.1)
        self.r.inject([FX[0]] * 200)
        fu, fw = u.frames(u.recv_all(0.5)), w.frames(w.recv_all(0.5))
        self.assertEqual(len(fu), 200); self.assertEqual(len(fw), 200)
        su = [f['seq'] for f in fu]; sw = [f['seq'] for f in fw]
        self.assertEqual(su, list(range(su[0], su[0] + 200))); self.assertEqual(sw, su)

    def test_ws_full_pass_stall_does_not_starve_udp(self):
        # F1: a WS client with a tiny SO_RCVBUF that never reads backs up
        # until even a flush-and-retry can't enqueue for it; a single pass
        # then charges the rest of that pass as drops in one step and stops
        # touching that client (no further per-frame ws_flush/enqueue) for
        # the rest of the pass. Regression target: the relay must still
        # answer a UDP HELLO promptly, and the stalled client's own
        # your_drops must reflect the drops once it does read.
        u, w = Udp(self.r), Ws(self.r, rcvbuf=2048)
        u.send(hello()); w.send(hello()); time.sleep(0.1); u.recv_all(0.1); w.recv_all(0.1)
        self.r.inject([FX[0]] * 500)      # >> WS_QCAP (64); w never recv()s meanwhile
        deadline = time.time() + 0.5
        u.send(hello())
        u.s.settimeout(0.5)
        got_status = False
        while time.time() < deadline:
            try:
                k, m = parse(u.s.recv(65536))
            except socket.timeout:
                break
            if k == 'S':
                got_status = True
                break
        self.assertTrue(got_status, "UDP HELLO reply did not arrive within 0.5 s")
        u.recv_all(0.2)                   # drain the rest so it doesn't leak into other tests
        w.send(hello())
        wst = w.statuses(w.recv_all(0.5))
        self.assertTrue(wst)
        self.assertGreater(wst[-1]['your_drops'], 0)

import base64, hashlib

class Ws:
    def __init__(self, relay, rcvbuf=None):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf: self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.s.connect(('127.0.0.1', relay.ws_port))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(('GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n'
                        'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
                        'Sec-WebSocket-Version: 13\r\n\r\n' % key).encode())
        resp = b''
        while b'\r\n\r\n' not in resp: resp += self.s.recv(1)
        want = base64.b64encode(hashlib.sha1(
            (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        assert b'101' in resp and want.encode() in resp, resp
        self.buf = b''

    def send(self, payload, op=2):
        mk = os.urandom(4)
        n = len(payload)
        h = bytes([0x80 | op, 0x80 | n]) if n < 126 else bytes([0x80 | op, 0x80 | 126]) + struct.pack('>H', n)
        self.s.sendall(h + mk + bytes(b ^ mk[i & 3] for i, b in enumerate(payload)))

    def _one(self):
        while True:
            if len(self.buf) >= 2:
                l, o = self.buf[1] & 0x7F, 2
                if l == 126 and len(self.buf) >= 4: l, o = struct.unpack('>H', self.buf[2:4])[0], 4
                elif l == 127 and len(self.buf) >= 10: l, o = struct.unpack('>Q', self.buf[2:10])[0], 10
                if l < 126 or o > 2:
                    if len(self.buf) >= o + l:
                        op, p = self.buf[0] & 0x0F, self.buf[o:o + l]
                        self.buf = self.buf[o + l:]
                        return op, p
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d

    def recv_all(self, quiet=0.3):
        out = []; self.s.settimeout(quiet)
        try:
            while True:
                op, p = self._one()
                if op == 2: out.append(parse(p))
        except (socket.timeout, EOFError): pass
        return out

    def frames(self, msgs): return [m for k, m in msgs if k == 'F']
    def statuses(self, msgs): return [m for k, m in msgs if k == 'S']
    def closed(self, wait=0.5):
        self.s.settimeout(wait)
        try:
            while True:
                d = self.s.recv(65536)
                if not d: return True
        except socket.timeout: return False
        except ConnectionResetError: return True

class WsTests(RelayTestBase):
    def test_ws_gets_same_frames_as_udp(self):
        u, w = Udp(self.r), Ws(self.r)
        u.send(hello()); w.send(hello()); time.sleep(0.1); u.recv_all(0.1); w.recv_all(0.1)
        self.r.inject(FX * 5)
        fu, fw = u.frames(u.recv_all()), w.frames(w.recv_all())
        self.assertEqual(len(fw), 15)
        self.assertEqual([(f['seq'], f['ch'], f['body'], f['flags'] & 1) for f in fu],
                         [(f['seq'], f['ch'], f['body'], f['flags'] & 1) for f in fw])

    def test_ws_alone_owns_and_can_tune(self):
        w = Ws(self.r); w.send(hello())
        st = w.statuses(w.recv_all())
        self.assertEqual((st[-1]['owner'], st[-1]['you_own']), (2, 1))
        w.send(tune(20, 149, 1))
        st = w.statuses(w.recv_all(0.5))
        self.assertEqual([(s['state'], s['tune_id']) for s in st][-2:], [(1, 20), (0, 20)])

    def test_ws_tune_refused_while_udp_alive_then_accepted(self):
        u, w = Udp(self.r), Ws(self.r)
        u.send(hello()); w.send(hello()); time.sleep(0.1); u.recv_all(0.1); w.recv_all(0.1)
        w.send(tune(21, 149, 1))
        st = w.statuses(w.recv_all())
        self.assertEqual([(s['state'], s['tune_id'], s['owner'], s['you_own']) for s in st],
                         [(3, 21, 1, 0)])
        for _ in range(6):                          # WS keeps alive, UDP lapses
            time.sleep(0.45); w.send(hello())
        w.recv_all(0.1)
        w.send(tune(22, 149, 1))
        self.assertEqual(w.statuses(w.recv_all(0.5))[-1]['state'], 0)

    def test_ws_without_hello_is_closed(self):
        w = Ws(self.r)
        time.sleep(2.5)
        self.assertTrue(w.closed())

    def test_third_ws_closed_at_accept(self):
        a, b = Ws(self.r), Ws(self.r)
        a.send(hello()); b.send(hello()); time.sleep(0.1)
        s = socket.create_connection(('127.0.0.1', self.r.ws_port))
        s.settimeout(1.0)
        try: data = s.recv(1)
        except ConnectionResetError: data = b''
        self.assertEqual(data, b'')

    def test_ws_slow_reader_does_not_hurt_udp(self):
        u, w = Udp(self.r), Ws(self.r, rcvbuf=4096)
        u.send(hello()); w.send(hello()); time.sleep(0.1); u.recv_all(0.1); w.recv_all(0.1)
        N = 3000
        fu = []
        for i in range(N // 100):                   # keep both alive while flooding
            self.r.inject([FX[0]] * 100)
            u.send(hello()); w.send(hello())
            fu += u.frames(u.recv_all(0.05))        # drain: rmem_max caps SO_RCVBUF
        fu += u.frames(u.recv_all(0.5))
        self.assertEqual(len(fu), N)
        seqs = [f['seq'] for f in fu]
        self.assertEqual(seqs, list(range(seqs[0], seqs[0] + N)))
        fw = w.frames(w.recv_all(0.5))
        self.assertLess(len(fw), N)
        last = fw[-1]['seq'] if fw else None
        self.r.inject([FX[0]] * 5); w.send(hello())
        fw2 = w.frames(w.recv_all(0.5))
        self.assertGreaterEqual(len(fw2), 1)
        self.assertTrue(fw2[0]['flags'] & 2)
        if last is not None: self.assertGreater(fw2[0]['seq'], last + 1)
        self.assertFalse(any(f['flags'] & 2 for f in fw2[1:]))
        w.send(hello())
        st = w.statuses(w.recv_all(0.5))
        self.assertGreater(st[-1]['your_drops'], 0)

    def test_ws_garbage_closes_only_that_client(self):
        u, w = Udp(self.r), Ws(self.r)
        u.send(hello()); w.send(hello()); time.sleep(0.1); u.recv_all(0.1)
        w.s.sendall(b'\x82\x05hello')               # unmasked client frame = protocol error
        self.assertTrue(w.closed())
        self.r.inject(FX[:1])
        self.assertEqual(len(u.frames(u.recv_all())), 1)

if __name__ == '__main__':
    unittest.main(verbosity=2)
