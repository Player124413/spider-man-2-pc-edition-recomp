import sys, collections, os
calls = collections.Counter(); byts = collections.Counter(); uniq = collections.defaultdict(set); size = {}
big = collections.Counter()
for ln in open(sys.argv[1]):
    if not ln.startswith("[read]"): continue
    _, path, off, n = ln.rsplit(" ", 3) if False else (None,) + tuple(ln[7:].rsplit(" ", 2))
    off, n = int(off), int(n)
    calls[path] += 1; byts[path] += n
    if n > 65536: big[path] += 1
    for b in range(off // 65536, (off + n - 1) // 65536 + 1): uniq[path].add(b)
tc = sum(calls.values()); tb = sum(byts.values())
print(f"total {tc} reads, {tb/1e6:.1f} MB, unique 64K blocks {sum(len(v) for v in uniq.values())} ({sum(len(v) for v in uniq.values())*65536/1e6:.1f} MB)")
for p, c in calls.most_common(15):
    print(f"{c:6} reads {byts[p]/1e6:7.1f} MB  uniq {len(uniq[p])*65536/1e6:6.1f} MB  big {big[p]:4}  {p.rsplit('/',1)[-1]}")
