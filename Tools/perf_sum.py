"""perf_sum.py log [every]  - one line per -GenPerf report: frame, fps, objects, step ms, the biggest items."""
import re, sys
path = sys.argv[1]
every = int(sys.argv[2]) if len(sys.argv) > 2 else 1
want = sys.argv[3].split(",") if len(sys.argv) > 3 else ["Obj.StartPath", "Path.Cells#", "Obj.FindAutoTarget", "AI", "Collisions"]
rep = {}
order = []
for line in open(path, encoding="utf-8", errors="replace"):
    m = re.search(r"\[perf f(\d+)\] (.*)", line)
    if not m:
        m2 = re.search(r"(Assertion failed.*|Fatal error.*)", line)
        if m2:
            print("!!", m2.group(1)[:160])
        continue
    f, rest = int(m.group(1)), m.group(2)
    r = rep.setdefault(f, {})
    if f not in order:
        order.append(f)
    if rest.startswith("logic"):
        r["logic"] = dict((k, (float(v), float(mx), float(n))) for k, v, mx, n in re.findall(r"(\S+)=([\d.]+)\(max ([\d.]+), x([\d.]+)\)", rest))
    elif rest.startswith("drawing"):
        r["draw"] = dict((k, (float(v), float(mx), float(n))) for k, v, mx, n in re.findall(r"(\S+)=([\d.]+)\(max ([\d.]+), x([\d.]+)\)", rest))
    elif rest.startswith("pacing"):
        m3 = re.search(r"over 4.2 ms: ([\d.]+)%, over 5 ms: ([\d.]+)%, over 8.3 ms: ([\d.]+)%; steps (on their own thread|on the game thread): up to ([\d.]+) ms.*?screen ([\d.]+) ms behind, held for a late step in (\d+) frames.*?gate ([\d.]+) ms a frame \(longest ([\d.]+) ms\)", rest)
        if m3:
            r["pace"] = m3.groups()
    elif rest.startswith("engine"):
        m3 = re.search(r"game thread ([\d.]+) ms, render thread ([\d.]+) ms, GPU ([\d.]+) ms, (\d+) draw calls.*over 33 ms: (\d+), over 50 ms: (\d+), over 100 ms: (\d+) \(speed x(\d+)\)(?:; memory (\d+) MB, (\d+) engine objects)?", rest)
        if m3:
            r["eng"] = m3.groups()
    else:
        m3 = re.search(r"(\d+) steps in ([\d.]+) s \((\d+)%.*?(\d+) frames: ([\d.]+) fps, frame ([\d.]+) ms avg / (\d+) max; objects (\d+) \(buildings (\d+), infantry (\d+), vehicles (\d+), aircraft (\d+)", rest)
        if m3:
            r["top"] = m3.groups()
for i, f in enumerate(order):
    r = rep[f]
    if i % every and not (r.get("eng") and r["eng"][7] == "1"):
        continue
    if "top" not in r:
        continue
    t = r["top"]
    e = r.get("eng")
    lg = r.get("logic", {})
    dr = r.get("draw", {})
    s = "f%6d x%s %6.1f fps (%3s%%) frame %5.1f/%3s ms | objs %4s b%s i%s v%s a%s" % (f, e[7] if e else "?", float(t[4]), t[2], float(t[5]), t[6], t[7], t[8], t[9], t[10], t[11])
    if e:
        s += " | GT %s RT %s GPU %s dc %s hitch %s/%s/%s" % (e[0], e[1], e[2], e[3], e[4], e[5], e[6])
        if e[8]:
            s += " mem %s MB objs %s" % (e[8], e[9])
    pc = r.get("pace")
    if pc:
        s += " | >4.2ms %s%% >5ms %s%% >8.3ms %s%% %s wall %s lag %s holds %s gate %s/%s" % (pc[0], pc[1], pc[2], "thr" if "own" in pc[3] else "gt", pc[4], pc[5], pc[6], pc[7], pc[8])
    if "Step" in lg:
        s += " | step %.2f (max %.0f)" % (lg["Step"][0], lg["Step"][1])
    for k in want:
        d = lg if k in lg else dr
        if k in d:
            s += " %s %.2f" % (k, d[k][0]) + ("" if k.endswith("#") else " x%.1f max %.0f" % (d[k][2], d[k][1]))
    print(s)
