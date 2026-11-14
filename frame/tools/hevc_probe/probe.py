#!/usr/bin/env python3
# Steam Link Pyrowave: read-only probe of the Frame's hardware video decoder (2026-11-14).
#
# 1. Lists every V4L2 video node (driver, card, capabilities), the coded and raw formats of the decoder, its frame
#    size limits per coded format, and all of its controls with their menu entries (HEVC profiles, levels, ...).
# 2. Decodes a few tiny HEVC test clips (12 frames each, a test pattern) through the decoder's normal stateful
#    V4L2 interface and checks the decoded brightness against references made on the PC:
#    4:2:0 8/10-bit at 768x3072 (baseline), Steam Link's stock 1344x5376, the taller/wider sizes a packed
#    4:4:4 or 4:2:2 stream would need, and native 4:2:2 / 4:4:4 (HEVC Range Extensions).
#
# Nothing is changed on the Frame: it only opens the decoder like any video player would, from user space, with
# its own private decoder session, and closes it again. No files are written outside /tmp/slp_hevc_probe.
# No account, session, key or network information is read or printed.
#
# Run it while NOT streaming (quit the Steam Link VR session first); it refuses to run while vrlink is running
# unless given --anyway.
import ctypes, errno, fcntl, json, mmap, os, select, sys, time
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = "/tmp/slp_hevc_probe"
lines = []

def log(s=""):
    print(s, flush=True)
    lines.append(s)

# ---------------------------------------------------------------- ioctl numbers
def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (ord(t) << 8) | nr
def _IOR(t, nr, size): return _IOC(2, t, nr, size)
def _IOW(t, nr, size): return _IOC(1, t, nr, size)
def _IOWR(t, nr, size): return _IOC(3, t, nr, size)

class v4l2_capability(ctypes.Structure):
    _fields_ = [("driver", ctypes.c_char * 16), ("card", ctypes.c_char * 32), ("bus_info", ctypes.c_char * 32),
                ("version", ctypes.c_uint32), ("capabilities", ctypes.c_uint32), ("device_caps", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 3)]

class v4l2_fmtdesc(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("type", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("description", ctypes.c_char * 32), ("pixelformat", ctypes.c_uint32), ("mbus_code", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 3)]

class v4l2_frmsizeenum(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("pixel_format", ctypes.c_uint32), ("type", ctypes.c_uint32),
                ("u", ctypes.c_uint32 * 6), ("reserved", ctypes.c_uint32 * 2)]

class v4l2_query_ext_ctrl(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint32), ("type", ctypes.c_uint32), ("name", ctypes.c_char * 32),
                ("minimum", ctypes.c_int64), ("maximum", ctypes.c_int64), ("step", ctypes.c_uint64),
                ("default_value", ctypes.c_int64), ("flags", ctypes.c_uint32), ("elem_size", ctypes.c_uint32),
                ("elems", ctypes.c_uint32), ("nr_of_dims", ctypes.c_uint32), ("dims", ctypes.c_uint32 * 4),
                ("reserved", ctypes.c_uint32 * 32)]

class v4l2_querymenu(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("id", ctypes.c_uint32), ("index", ctypes.c_uint32), ("name", ctypes.c_char * 32),
                ("reserved", ctypes.c_uint32)]

class v4l2_plane_pix_format(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("sizeimage", ctypes.c_uint32), ("bytesperline", ctypes.c_uint32), ("reserved", ctypes.c_uint16 * 6)]

class v4l2_pix_format_mplane(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("pixelformat", ctypes.c_uint32),
                ("field", ctypes.c_uint32), ("colorspace", ctypes.c_uint32),
                ("plane_fmt", v4l2_plane_pix_format * 8), ("num_planes", ctypes.c_uint8), ("flags", ctypes.c_uint8),
                ("ycbcr_enc", ctypes.c_uint8), ("quantization", ctypes.c_uint8), ("xfer_func", ctypes.c_uint8),
                ("reserved", ctypes.c_uint8 * 7)]

class _fmt_union(ctypes.Union):
    _fields_ = [("pix_mp", v4l2_pix_format_mplane), ("raw_data", ctypes.c_uint8 * 200), ("_align", ctypes.c_uint64)]

class v4l2_format(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("fmt", _fmt_union)]

class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32), ("type", ctypes.c_uint32), ("memory", ctypes.c_uint32),
                ("capabilities", ctypes.c_uint32), ("flags", ctypes.c_uint8), ("reserved", ctypes.c_uint8 * 3)]

class v4l2_plane(ctypes.Structure):
    _fields_ = [("bytesused", ctypes.c_uint32), ("length", ctypes.c_uint32), ("mem_offset", ctypes.c_uint64),
                ("data_offset", ctypes.c_uint32), ("reserved", ctypes.c_uint32 * 11)]

class timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_int64), ("tv_usec", ctypes.c_int64)]

class v4l2_timecode(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("frames", ctypes.c_uint8),
                ("seconds", ctypes.c_uint8), ("minutes", ctypes.c_uint8), ("hours", ctypes.c_uint8),
                ("userbits", ctypes.c_uint8 * 4)]

class v4l2_buffer(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("type", ctypes.c_uint32), ("bytesused", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("field", ctypes.c_uint32), ("timestamp", timeval),
                ("timecode", v4l2_timecode), ("sequence", ctypes.c_uint32), ("memory", ctypes.c_uint32),
                ("planes", ctypes.c_void_p), ("length", ctypes.c_uint32), ("reserved2", ctypes.c_uint32),
                ("request_fd", ctypes.c_int32)]

class v4l2_event_subscription(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("id", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 5)]

class timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_int64), ("tv_nsec", ctypes.c_int64)]

class v4l2_event(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("u", ctypes.c_uint64 * 8), ("pending", ctypes.c_uint32),
                ("sequence", ctypes.c_uint32), ("timestamp", timespec), ("id", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 8)]

class v4l2_control(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint32), ("value", ctypes.c_int32)]

class v4l2_decoder_cmd(ctypes.Structure):
    _fields_ = [("cmd", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("data", ctypes.c_uint32 * 16)]

for s, n in ((v4l2_capability, 104), (v4l2_fmtdesc, 64), (v4l2_frmsizeenum, 44), (v4l2_query_ext_ctrl, 232),
             (v4l2_querymenu, 44), (v4l2_format, 208), (v4l2_requestbuffers, 20), (v4l2_plane, 64),
             (v4l2_buffer, 88), (v4l2_event_subscription, 32), (v4l2_event, 136), (v4l2_decoder_cmd, 72)):
    assert ctypes.sizeof(s) == n, (s.__name__, ctypes.sizeof(s), n)

QUERYCAP = _IOR('V', 0, 104); ENUM_FMT = _IOWR('V', 2, 64); G_FMT = _IOWR('V', 4, 208); S_FMT = _IOWR('V', 5, 208)
REQBUFS = _IOWR('V', 8, 20); QUERYBUF = _IOWR('V', 9, 88); QBUF = _IOWR('V', 15, 88); DQBUF = _IOWR('V', 17, 88)
STREAMON = _IOW('V', 18, 4); STREAMOFF = _IOW('V', 19, 4); G_CTRL = _IOWR('V', 27, 8)
QUERYMENU = _IOWR('V', 37, 44); ENUM_FRAMESIZES = _IOWR('V', 74, 44); DQEVENT = _IOR('V', 89, 136)
SUBSCRIBE_EVENT = _IOW('V', 90, 32); DECODER_CMD = _IOWR('V', 96, 72); QUERY_EXT_CTRL = _IOWR('V', 103, 232)

OUT_MP, CAP_MP = 10, 9            # V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE / CAPTURE_MPLANE
MEMORY_MMAP = 1
EV_EOS, EV_SOURCE_CHANGE = 2, 5
BUF_FLAG_ERROR, BUF_FLAG_LAST = 0x40, 0x100000
CID_MIN_BUFFERS_FOR_CAPTURE = 0x00980927
CTRL_FLAG_NEXT_CTRL, CTRL_FLAG_NEXT_COMPOUND = 0x80000000, 0x40000000
CTRL_TYPE_MENU, CTRL_TYPE_INTEGER_MENU = 3, 9
CAP_M2M_MPLANE = 0x00004000

def fourcc(v): return "".join(chr((v >> (8 * i)) & 0xff) for i in range(4))
def fcc(s): return sum(ord(c) << (8 * i) for i, c in enumerate(s))
HEVC = fcc("HEVC")

def ioctl(fd, req, arg):
    fcntl.ioctl(fd, req, arg, True)

# ---------------------------------------------------------------- part 1: capabilities
def enum_formats(fd, typ):
    out, i = [], 0
    while True:
        d = v4l2_fmtdesc(index=i, type=typ)
        try: ioctl(fd, ENUM_FMT, d)
        except OSError: break
        out.append((d.pixelformat, d.description.decode(errors="replace"), d.flags))
        i += 1
    return out

def frame_sizes(fd, pf):
    out, i = [], 0
    while True:
        e = v4l2_frmsizeenum(index=i, pixel_format=pf)
        try: ioctl(fd, ENUM_FRAMESIZES, e)
        except OSError: break
        if e.type == 1: out.append("%ux%u" % (e.u[0], e.u[1]))
        else:
            out.append("%u..%u (step %u) x %u..%u (step %u)" % (e.u[0], e.u[1], e.u[2], e.u[3], e.u[4], e.u[5]))
            break
        i += 1
    return out

def list_controls(fd):
    q = v4l2_query_ext_ctrl(id=CTRL_FLAG_NEXT_CTRL | CTRL_FLAG_NEXT_COMPOUND)
    while True:
        try: ioctl(fd, QUERY_EXT_CTRL, q)
        except OSError: break
        name = q.name.decode(errors="replace")
        s = "    0x%08x %-34s type %u  min %d max %d step %d default %d" % (q.id, name, q.type, q.minimum, q.maximum,
                                                                         q.step, q.default_value)
        if q.flags & 0x0001: s += " (disabled)"
        log(s)
        if q.type in (CTRL_TYPE_MENU, CTRL_TYPE_INTEGER_MENU):
            for idx in range(max(0, q.minimum), min(q.maximum, 64) + 1):
                m = v4l2_querymenu(id=q.id, index=idx)
                try: ioctl(fd, QUERYMENU, m)
                except OSError: continue
                if q.type == CTRL_TYPE_MENU: label = m.name.split(b"\0")[0].decode(errors="replace")
                else: label = str(int.from_bytes(bytes(m.name)[:8], "little", signed=True))
                log("        [%d] %s" % (idx, label))
        q.id |= CTRL_FLAG_NEXT_CTRL | CTRL_FLAG_NEXT_COMPOUND

def find_decoder():
    found = None
    log("== Video nodes")
    for n in sorted(os.listdir("/dev")):
        if not n.startswith("video"): continue
        p = "/dev/" + n
        try: fd = os.open(p, os.O_RDWR | os.O_NONBLOCK)
        except OSError as e:
            log("  %s: cannot open (%s)" % (p, e.strerror)); continue
        try:
            c = v4l2_capability(); ioctl(fd, QUERYCAP, c)
            caps = c.device_caps or c.capabilities
            coded = [f for f in enum_formats(fd, OUT_MP)] if caps & CAP_M2M_MPLANE else []
            log("  %s: driver %s, card %s, caps 0x%08x, OUTPUT formats %s" % (
                p, c.driver.decode(), c.card.decode(), caps, ", ".join(fourcc(f[0]) for f in coded) or "-"))
            if found is None and any(f[0] == HEVC for f in coded):
                found = p
        except OSError as e:
            log("  %s: QUERYCAP failed (%s)" % (p, e.strerror))
        finally:
            os.close(fd)
    return found

def describe_decoder(path):
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    try:
        log("\n== Decoder %s" % path)
        log("  coded formats (OUTPUT):")
        for pf, desc, fl in enum_formats(fd, OUT_MP):
            log("    %s  %-28s flags 0x%x  sizes %s" % (fourcc(pf), desc, fl, "; ".join(frame_sizes(fd, pf)) or "?"))
        log("  raw formats (CAPTURE, before a stream is set):")
        for pf, desc, fl in enum_formats(fd, CAP_MP):
            log("    %s  %-28s flags 0x%x" % (fourcc(pf), desc, fl))
        log("  controls:")
        list_controls(fd)
    finally:
        os.close(fd)

# ---------------------------------------------------------------- part 2: decode tests
def split_access_units(data):
    """Annex-B HEVC -> list of access units (bytes)."""
    starts, i, n = [], 0, len(data)
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0: break
        s = j - 1 if j > 0 and data[j - 1] == 0 else j
        starts.append((s, j + 3)); i = j + 3
    nals = []
    for k, (s, p) in enumerate(starts):
        e = starts[k + 1][0] if k + 1 < len(starts) else n
        nals.append((s, e, (data[p] >> 1) & 0x3f, data[p + 2] if p + 2 < n else 0))
    aus, cur_start, has_vcl = [], None, False
    for s, e, t, b2 in nals:
        vcl = t < 32
        new = (not vcl and t in (32, 33, 34, 35, 39) and has_vcl) or (vcl and (b2 & 0x80) and has_vcl)
        if cur_start is None: cur_start = s
        elif new:
            aus.append(data[cur_start:s]); cur_start = s; has_vcl = False
        if vcl: has_vcl = True
    if cur_start is not None: aus.append(data[cur_start:])
    return aus

def block_means(buf, offset, stride, w, h, sixteen):
    out = []
    if sixteen:
        a = array("H"); a.frombytes(bytes(buf[offset:offset + stride * h]))
        if sys.byteorder != "little": a.byteswap()
        st = stride // 2
        for by in range(h // 16):
            acc = [0] * (w // 16)
            for y in range(by * 16, by * 16 + 16):
                r = y * st
                for bx in range(w // 16):
                    acc[bx] += sum(a[r + bx * 16:r + bx * 16 + 16])
            out += [v / 256.0 / 64.0 / 4.0 for v in acc]   # P010-style: 10 bits in the top -> 8-bit scale
    else:
        for by in range(h // 16):
            acc = [0] * (w // 16)
            for y in range(by * 16, by * 16 + 16):
                row = buf[offset + y * stride:offset + y * stride + w]
                for bx in range(w // 16):
                    acc[bx] += sum(row[bx * 16:bx * 16 + 16])
            out += [v / 256.0 for v in acc]
    return out

LINEAR_8 = {"NV12", "NM12", "NV16", "NV61", "NV24", "NV42", "YM24", "YU12", "YM12", "422P", "YM16", "GREY"}
LINEAR_16 = {"P010", "P210", "P410", "P012", "Y210", "Y410"}   # (Y210/Y410 are packed: no check, listed for info)

class DecodeTest:
    def __init__(self, path, clip, ref):
        self.path, self.clip, self.ref = path, clip, ref
        self.fd = None; self.out_maps = []; self.cap_maps = []; self.result = {}

    def close(self):
        if self.fd is None: return
        for t in (OUT_MP, CAP_MP):
            try: ioctl(self.fd, STREAMOFF, ctypes.c_int(t))
            except OSError: pass
        for m in self.out_maps + [x for c in self.cap_maps for x in c]:
            try: m.close()
            except Exception: pass
        for t in (OUT_MP, CAP_MP):
            try: ioctl(self.fd, REQBUFS, v4l2_requestbuffers(count=0, type=t, memory=MEMORY_MMAP))
            except OSError: pass
        os.close(self.fd); self.fd = None

    def qbuf_out(self, idx, data, n):
        planes = (v4l2_plane * 1)()
        planes[0].bytesused = len(data); planes[0].length = len(self.out_maps[idx])
        self.out_maps[idx][:len(data)] = data
        b = v4l2_buffer(index=idx, type=OUT_MP, memory=MEMORY_MMAP, length=1,
                        planes=ctypes.cast(planes, ctypes.c_void_p))
        b.timestamp.tv_sec = n; b.timestamp.tv_usec = 0
        ioctl(self.fd, QBUF, b)

    def dqbuf(self, typ, nplanes):
        planes = (v4l2_plane * max(1, nplanes))()
        b = v4l2_buffer(type=typ, memory=MEMORY_MMAP, length=max(1, nplanes),
                        planes=ctypes.cast(planes, ctypes.c_void_p))
        try: ioctl(self.fd, DQBUF, b)
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EPIPE): return None, None
            raise
        return b, planes

    def setup_capture(self):
        f = v4l2_format(type=CAP_MP); ioctl(self.fd, G_FMT, f)
        pm = f.fmt.pix_mp
        native = fourcc(pm.pixelformat)
        self.result["decoder picks"] = "%s %ux%u, %u plane(s)" % (native, pm.width, pm.height, pm.num_planes)
        # try a linear format we can read: same bit depth, 4:2:0 first
        if native not in LINEAR_8 and native not in LINEAR_16:
            for want in (("P010", "NV12") if self.ref["bits"] == 10 else ("NV12",)):
                g = v4l2_format(type=CAP_MP); ioctl(self.fd, G_FMT, g)
                g.fmt.pix_mp.pixelformat = fcc(want)
                try:
                    ioctl(self.fd, S_FMT, g)
                    if fourcc(g.fmt.pix_mp.pixelformat) == want: break
                except OSError: pass
        f = v4l2_format(type=CAP_MP); ioctl(self.fd, G_FMT, f)
        pm = f.fmt.pix_mp
        self.cap_fmt = fourcc(pm.pixelformat); self.cap_w, self.cap_h = pm.width, pm.height
        self.cap_planes = pm.num_planes; self.cap_stride = pm.plane_fmt[0].bytesperline
        self.result["capture format used"] = "%s %ux%u stride %u, %u plane(s)" % (
            self.cap_fmt, pm.width, pm.height, self.cap_stride, pm.num_planes)
        ctl = v4l2_control(id=CID_MIN_BUFFERS_FOR_CAPTURE)
        try: ioctl(self.fd, G_CTRL, ctl); nmin = ctl.value
        except OSError: nmin = 4
        rb = v4l2_requestbuffers(count=max(nmin, 1) + 2, type=CAP_MP, memory=MEMORY_MMAP)
        ioctl(self.fd, REQBUFS, rb)
        for i in range(rb.count):
            planes = (v4l2_plane * self.cap_planes)()
            b = v4l2_buffer(index=i, type=CAP_MP, memory=MEMORY_MMAP, length=self.cap_planes,
                            planes=ctypes.cast(planes, ctypes.c_void_p))
            ioctl(self.fd, QUERYBUF, b)
            maps = [mmap.mmap(self.fd, planes[p].length, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                              offset=planes[p].mem_offset & 0xffffffff) for p in range(self.cap_planes)]
            self.cap_maps.append(maps)
            ioctl(self.fd, QBUF, b)
        ioctl(self.fd, STREAMON, ctypes.c_int(CAP_MP))

    def check_frame(self, b, planes, n):
        if self.cap_fmt in LINEAR_8 or self.cap_fmt in LINEAR_16:
            if self.cap_w < self.ref["w"] or self.cap_h < self.ref["h"]:
                return "size %ux%u is smaller than the clip" % (self.cap_w, self.cap_h)
            refm = self.ref["blockmeans"].get(str(n))
            if refm is None: return None
            got = block_means(self.cap_maps[b.index][0], planes[0].data_offset, self.cap_stride, self.ref["w"],
                              self.ref["h"], self.cap_fmt in LINEAR_16)
            diff = max(abs(a - r) for a, r in zip(got, refm))
            return "brightness max block error %.2f (of 255) -> %s" % (diff, "MATCH" if diff < 1.0 else "WRONG")
        return "not checked (format %s is not a linear one this probe reads)" % self.cap_fmt

    def run(self):
        aus = split_access_units(open(os.path.join(HERE, "clips", self.clip + ".hevc"), "rb").read())
        self.result["access units"] = len(aus)
        self.fd = os.open(self.path, os.O_RDWR | os.O_NONBLOCK)
        f = v4l2_format(type=OUT_MP)
        f.fmt.pix_mp.width, f.fmt.pix_mp.height = self.ref["w"], self.ref["h"]
        f.fmt.pix_mp.pixelformat = HEVC; f.fmt.pix_mp.num_planes = 1
        f.fmt.pix_mp.plane_fmt[0].sizeimage = max(2 << 20, max(len(a) for a in aus) + 4096)
        try: ioctl(self.fd, S_FMT, f)
        except OSError as e:
            self.result["verdict"] = "REFUSED at S_FMT (%s)" % e.strerror; return
        for ev in (EV_SOURCE_CHANGE, EV_EOS):
            try: ioctl(self.fd, SUBSCRIBE_EVENT, v4l2_event_subscription(type=ev))
            except OSError: pass
        rb = v4l2_requestbuffers(count=4, type=OUT_MP, memory=MEMORY_MMAP); ioctl(self.fd, REQBUFS, rb)
        for i in range(rb.count):
            planes = (v4l2_plane * 1)()
            b = v4l2_buffer(index=i, type=OUT_MP, memory=MEMORY_MMAP, length=1,
                            planes=ctypes.cast(planes, ctypes.c_void_p))
            ioctl(self.fd, QUERYBUF, b)
            self.out_maps.append(mmap.mmap(self.fd, planes[0].length, mmap.MAP_SHARED,
                                           mmap.PROT_READ | mmap.PROT_WRITE, offset=planes[0].mem_offset & 0xffffffff))
        free = list(range(len(self.out_maps)))
        nxt, decoded, errors, checks, cap_ready, stopped = 0, 0, 0, [], False, False
        ioctl(self.fd, STREAMON, ctypes.c_int(OUT_MP))
        deadline = time.monotonic() + 10.0
        poller = select.poll(); poller.register(self.fd, select.POLLIN | select.POLLOUT | select.POLLPRI | select.POLLERR)
        while time.monotonic() < deadline:
            while free and nxt < len(aus):
                self.qbuf_out(free.pop(), aus[nxt], nxt); nxt += 1
            if nxt == len(aus) and not stopped and cap_ready:
                try: ioctl(self.fd, DECODER_CMD, v4l2_decoder_cmd(cmd=1))   # STOP: drain
                except OSError: pass
                stopped = True
            ev = poller.poll(200)
            if not ev: continue
            mask = ev[0][1]
            if mask & select.POLLPRI:
                e = v4l2_event()
                try: ioctl(self.fd, DQEVENT, e)
                except OSError: e = None
                if e is not None and e.type == EV_SOURCE_CHANGE and not cap_ready:
                    self.setup_capture(); cap_ready = True
            while True:
                b, _ = self.dqbuf(OUT_MP, 1)
                if b is None: break
                free.append(b.index)
            if cap_ready:
                done = False
                while True:
                    b, planes = self.dqbuf(CAP_MP, self.cap_planes)
                    if b is None: break
                    if b.flags & BUF_FLAG_ERROR: errors += 1
                    elif planes[0].bytesused > 0:
                        n = b.timestamp.tv_sec
                        if str(n) in self.ref["blockmeans"]:
                            checks.append("frame %d: %s" % (n, self.check_frame(b, planes, n)))
                        decoded += 1
                    if b.flags & BUF_FLAG_LAST: done = True; break
                    ioctl(self.fd, QBUF, b)
                if done or decoded + errors >= len(aus): break
        self.result["frames decoded"] = "%d of %d (%d with the error flag)" % (decoded, len(aus), errors)
        if checks: self.result["checks"] = checks
        if not cap_ready: v = "NO PICTURE: the decoder never reported the stream format (not supported?)"
        elif decoded == 0: v = "NO FRAMES decoded"
        elif any("WRONG" in c for c in checks): v = "DECODES, but the picture is WRONG"
        elif checks and all("MATCH" in c for c in checks): v = "OK (picture matches the PC's decode)"
        else: v = "DECODES (picture not checked)"
        self.result["verdict"] = v

def vrlink_running():
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try:
            if os.path.basename(os.readlink("/proc/%s/exe" % p)) == "vrlink": return True
        except OSError: pass
    return False

def main():
    if vrlink_running() and "--anyway" not in sys.argv:
        print("vrlink (the Steam Link VR client) is running. Quit the streaming session first, then run this again\n"
              "(or add --anyway to run it next to vrlink; it would only open its own decoder session).")
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    log("Steam Link Pyrowave HEVC decoder probe, %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    try: log("kernel %s" % os.uname().release)
    except Exception: pass
    dec = find_decoder()
    if not dec:
        log("\nNo V4L2 node with an HEVC decoder found."); return 1
    describe_decoder(dec)
    refs = json.load(open(os.path.join(HERE, "refs.json")))
    log("\n== Decode tests (12 frames each, I-frame every 6)")
    for clip in sorted(refs, key=lambda c: (c.split("_")[0], int(c.split("_")[1]), c)):
        t = DecodeTest(dec, clip, refs[clip])
        t0 = time.monotonic()
        try: t.run()
        except Exception as e: t.result["verdict"] = "FAILED: %s: %s" % (type(e).__name__, e)
        finally: t.close()
        r = refs[clip]
        log("  %s (%s %d-bit %ux%u): %s  [%.1f s]" % (clip, r["chroma"], r["bits"], r["w"], r["h"],
                                                   t.result.get("verdict", "?"), time.monotonic() - t0))
        for k in ("decoder picks", "capture format used", "frames decoded", "access units"):
            if k in t.result: log("      %s: %s" % (k, t.result[k]))
        for c in t.result.get("checks", []): log("      " + c)
    with open(os.path.join(OUT_DIR, "report.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    log("\nReport written to %s/report.txt" % OUT_DIR)
    return 0

if __name__ == "__main__":
    sys.exit(main())
