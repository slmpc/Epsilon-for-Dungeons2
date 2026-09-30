"""Discover engine-subclass instances (GEngine) and confirm GWorld."""
import sys, os, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import enum_procs, Proc, pe_sections

LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'engine2_log.txt')
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
    secs, sizeimg, hdr = pe_sections(pr, base)
    secmap = {n: (va, vs) for n, va, vs, _, _ in secs}
    blocks = base + GNAMES_RVA + 0x10

    def resolve(index):
        b = pr.u64(blocks + 8 * (index >> 16))
        if not b:
            return None
        p = b + (index & 0xFFFF) * 2
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

    objs = {}
    by_class = {}
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
        nm = resolve(struct.unpack_from('<i', oh, 0x18)[0])
        objs[obj] = (nm, cls)
        by_class.setdefault(cls, []).append(obj)

    cname = {}
    def cn(c):
        if c in cname:
            return cname[c]
        r = None
        oh = pr.read(c, 0x28) if c else None
        if oh:
            r = resolve(struct.unpack_from('<i', oh, 0x18)[0])
        cname[c] = r
        return r

    # 所有类名含 Engine / GameInstance / GameViewport / World 的 UClass
    interesting = {}
    for obj, (nm, cls) in objs.items():
        if nm and cn(cls) == 'Class':
            if any(k in nm for k in ('Engine', 'GameInstance', 'GameViewport', 'World')):
                interesting[nm] = obj
    log('=== 相关 UClass（名字含 Engine/GameInstance/GameViewport/World）===')
    inst_of = {}
    for nm, ca in sorted(interesting.items()):
        insts = [a for a in by_class.get(ca, []) if not (objs[a][0] or '').startswith('Default__')]
        if insts:
            log('   %-42s UClass=%#012x 非CDO实例=%d' % (nm, ca, len(insts)))
        for a in insts:
            inst_of[a] = nm

    if not inst_of:
        log('   没有任何非 CDO 实例（游戏可能还没进主循环）')

    log('=== .data 中指向这些实例的全局槽 ===')
    dbase, dsize = secmap['.data']
    off = 0
    while off < dsize:
        n = min(0x100000, dsize - off)
        buf = pr.read(dbase + off, n)
        off += n
        if not buf:
            continue
        for k in range(0, len(buf) - 8, 8):
            v = struct.unpack_from('<Q', buf, k)[0]
            if v in inst_of:
                slot = dbase + off - n + k
                log('   global %#012x (rva %#-9x) -> %-40s  (%#x)'
                    % (slot, slot - base, inst_of[v], v))

    # 子类关系：找 'Engine' UClass 的派生类
    log('=== UEngine 的派生类（直接/间接）===')
    eng_cls = None
    for obj, (nm, cls) in objs.items():
        if nm == 'Engine' and cn(cls) == 'Class':
            eng_cls = obj
            break
    if eng_cls:
        childs = [a for a in by_class.get(eng_cls, [])]
        log('   Engine UClass = %#x' % eng_cls)
        for a in childs[:10]:
            log('     实例/子对象 %#012x %r' % (a, objs[a][0]))
    # 用 SuperStruct 找派生类：UStruct::SuperStruct 通常在 +0x40 左右，这里只报告类名匹配
    log('=== 全部类名包含 "Game" 的类 ===')
    names = sorted(nm for nm in (v[0] for v in objs.values())
                   if nm and 'Game' in nm and cn(objs[[o for o, vv in objs.items() if vv[0] == nm][0]][1]) == 'Class')
    log('   ' + ', '.join(names[:80]))


main()
