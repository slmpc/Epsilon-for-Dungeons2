"""Enumerate all UObjects (with names) and back-search .data for global pointers to them.
This reveals GEngine / GWorld / other engine statics directly."""
import sys, os, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import enum_procs, Proc, pe_sections

LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'globals_log.txt')
GOBJECTS_RVA = 0x0BEA8BF0
GNAMES_RVA = 0x0BDC5040


def log(m):
    s = time.strftime('%H:%M:%S ') + m
    try:
        print(s, flush=True)
    except UnicodeEncodeError:
        print(s.encode('ascii', 'replace').decode(), flush=True)
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write(s + '\n')


def main():
    pr = pid = base = size = None
    for n, p in enum_procs():
        if n.lower() != 'dungeons-win64-shipping.exe':
            continue
        try:
            q = Proc(p)
            m = [x for x in q.modules() if x[0].lower() == 'dungeons-win64-shipping.exe']
            if m:
                pr, pid, base, size = q, p, m[0][1], m[0][2]
                break
        except Exception:
            continue
    if pr is None:
        log('no proc'); return
    log('pid=%d base=%#x' % (pid, base))
    secs, sizeimg, hdr = pe_sections(pr, base)
    secmap = {n: (va, vs) for n, va, vs, _, _ in secs}

    blocks = base + GNAMES_RVA + 0x10

    def resolve(index):
        blk = index >> 16
        off = index & 0xFFFF
        b = pr.u64(blocks + 8 * blk)
        if not b:
            return None
        p = b + off * 2
        h = pr.read(p, 2)
        if not h:
            return None
        hh = struct.unpack('<H', h)[0]
        iswide = hh & 1
        ln = (hh >> 6) & 0x3FF
        if not (0 < ln <= 500):
            return None
        s = pr.read(p + 2, ln * (2 if iswide else 1) + 2)
        if not s:
            return None
        try:
            return s[:ln * (2 if iswide else 1)].decode('utf-16-le' if iswide else 'latin1')
        except Exception:
            return None

    g = base + GOBJECTS_RVA
    tbl = pr.u64(g + 0x10)
    num = pr.u32(g + 0x24)
    log('GObjects=%#x NumElements=%d' % (g, num))

    objs = {}     # address -> (name, class_name)
    t0 = time.time()
    for idx in range(num):
        ch = pr.u64(tbl + 8 * (idx // 65536))
        if not ch:
            continue
        obj = pr.u64(ch + (idx % 65536) * 0x18)
        if not obj:
            continue
        oh = pr.read(obj, 0x28)
        if not oh:
            continue
        cls = struct.unpack_from('<Q', oh, 0x10)[0]
        nm = struct.unpack_from('<i', oh, 0x18)[0]
        objs[obj] = (resolve(nm), cls)
    log('枚举 %d 个对象 (%.1fs), 已解析名字 %d' % (num, time.time() - t0,
        sum(1 for v in objs.values() if v[0])))

    # 类名缓存
    clsname = {}
    def cname(c):
        if c in clsname:
            return clsname[c]
        oh = pr.read(c, 0x28) if c else None
        r = resolve(struct.unpack_from('<i', oh, 0x18)[0]) if oh else None
        clsname[c] = r
        return r

    # 扫描 .data 里指向 UObject 的全局槽
    dbase, dsize = secmap['.data']
    log('=== .data 中指向 UObject 的全局槽 ===')
    hits = []
    off = 0
    while off < dsize:
        n = min(0x100000, dsize - off)
        buf = pr.read(dbase + off, n)
        off += n
        if not buf:
            continue
        for k in range(0, len(buf) - 8, 8):
            v = struct.unpack_from('<Q', buf, k)[0]
            if v in objs:
                hits.append((dbase + off - n + k, v))
    log('命中 %d 个槽' % len(hits))
    for slot, v in hits:
        nm, cls = objs[v]
        log('   global %#012x (rva %#-9x) -> obj %#012x  %-40s : %s'
            % (slot, slot - base, v, nm, cname(cls)))

    # 按名字过滤出关键引擎全局
    log('=== 关键全局 ===')
    KEY = ('Engine', 'World', 'Package', 'GameInstance', 'PlayerController',
           'GameViewport', 'Level', 'GameState', 'LocalPlayer', 'NetDriver')
    for slot, v in hits:
        nm, cls = objs[v]
        if nm and (nm in KEY or cls and cname(cls) in KEY):
            log('   %#012x (rva %#x) -> %r : %r' % (slot, slot - base, nm, cname(cls)))


main()
