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

def hdr(t): return struct.pack('<HBB', 0x524D, 1, t)
def hello(): return hdr(HELLO)
def tune(tid, ch, sec): return hdr(TUNE) + struct.pack('<HBB', tid, ch, sec)

def parse(msg):
    magic, ver, t = struct.unpack('<HBB', msg[:4])
    assert magic == 0x524D and ver == 1, msg[:4]
    if t == FRAME:
        seq, ch, sec, fl = struct.unpack('<IBBB', msg[4:11])
        return ('F', dict(seq=seq, ch=ch, sec=sec, flags=fl, body=msg[11:]))
    if t == STATUS:
        f = struct.unpack('<HBBBBBIIIIII', msg[4:35])
        keys = ['tune_id', 'state', 'ch', 'sec', 'owner', 'you_own',
                'rx', 'fwd', 'foreign', 'bad_fcs', 'your_drops', 'uptime_s']
        return ('S', dict(zip(keys, f)))
    return ('?', t)

def free_port(kind):
    s = socket.socket(socket.AF_INET, kind); s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]; s.close(); return p

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
            self.assertEqual(bodies, [FX[0], FX[1], FX[3]])   # foreign (2) dropped
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
        self.assertEqual([f['body'] for f in fr], [FX[0]])

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

if __name__ == '__main__':
    unittest.main(verbosity=2)
