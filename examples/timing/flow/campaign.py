#!/usr/bin/env python3
# Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
"""Benchmark campaign: timing-aware synthesis against the best stock Yosys configuration of every design.

usage: campaign.py <stage>...

  stock   all stock configurations, each asked for minimum delay
  speed   synth_timing without a period (fastest), at the default effort and at effort 0
  sweep   synth_timing for periods of 1.05 to 3 times the fastest one
  limits  max slew / max capacitance violations counted by OpenSTA
  verify  formal and simulation based equivalence of the timing-aware netlists
  pnr     placement and repair by OpenROAD of all stock and timing-aware speed netlists
  report  markdown tables from the results

Stages are resumable: finished runs are kept in results/<stage>.jsonl.
"""
import importlib.util, json, math, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")
GEN = os.path.join(ROOT, "flow", "generated")
YOSYS = os.environ.get("YOSYS", os.path.join(ROOT, "snap", "yosys"))
DESIGNS = [l.split("\t") for l in open(os.path.join(ROOT, "bench", "designs.tsv")).read().splitlines()]
NAMES = [d[0] for d in DESIGNS]
HOLDOUT = {"aes", "ethmac"}  # not used when the mapping recipes were selected
FACTORS = [1.05, 1.2, 1.5, 2.0, 3.0]


def stock_configs():
    """name -> (synth options, abc options)"""
    constr = "-constr @OUT@/abc.constr"
    cfg = {
        "plain": ("", ""),
        "constr": ("", constr),
        "constr_d": ("", "-D 1 " + constr),
        "orfs": ("", "-D 1 %s -script %s/flow/orfs.abc" % (constr, ROOT)),
    }
    import librelane
    path = os.path.join(os.path.dirname(librelane.__file__), "scripts", "pyosys", "construct_abc_script.py")
    spec = importlib.util.spec_from_file_location("construct_abc_script", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    os.makedirs(GEN, exist_ok=True)
    creator = mod.ABCScriptCreator({
        "CLOCK_PERIOD": 0.001, "MAX_FANOUT_CONSTRAINT": 10, "MAX_TRANSITION_CONSTRAINT": 0.75,
        "SYNTH_ABC_LEGACY_REFACTOR": False, "SYNTH_ABC_LEGACY_REWRITE": False, "SYNTH_ABC_USE_MFS3": False,
        "SYNTH_ABC_AREA_USE_NF": False, "SYNTH_ABC_BUFFERING": True, "SYNTH_SIZING": True})
    script = {}
    for kind in ("AREA", "DELAY"):
        for n in range(5):
            name = "%s%d" % (kind.lower(), n)
            script[name] = "-D 1 %s -script %s" % (constr, creator.generate_abc_script(GEN, "%s %d" % (kind, n)))
            cfg["ll_" + name] = ("", script[name])
    ks = "-extra-map +/choices/kogge-stone.v"
    cfg["ks_area3"] = (ks, script["area3"])
    cfg["ks_delay4"] = (ks, script["delay4"])
    cfg["booth_ks_area3"] = ("-booth " + ks, script["area3"])
    return cfg


def load(stage):
    path = os.path.join(RES, stage + ".jsonl")
    rows = [json.loads(l) for l in open(path)] if os.path.exists(path) else []
    return {(r["design"], r["tag"]): r for r in rows}


def parallel(stage, jobs, func, n_par):
    done = load(stage)
    todo = [j for j in jobs if (j[0], j[1]) not in done]
    with open(os.path.join(RES, stage + ".jsonl"), "a") as out, ThreadPoolExecutor(n_par) as ex:
        futs = {ex.submit(func, *j): j for j in todo}
        for i, fut in enumerate(as_completed(futs), 1):
            row = dict(fut.result(), design=futs[fut][0], tag=futs[fut][1])
            out.write(json.dumps(row) + "\n")
            out.flush()
            print("[%s %d/%d] %s" % (stage, i, len(todo), row), flush=True)


def sh(cmd, timeout, **env):
    e = dict(os.environ, YOSYS=YOSYS, FY=YOSYS, **env)
    try:
        return subprocess.run(cmd, env=e, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, universal_newlines=True, errors="replace").stdout
    except subprocess.TimeoutExpired:
        return "status=TIMEOUT"


def fields(line):
    return dict(x.split("=", 1) for x in line.split() if "=" in x)


def synth(design, tag, flow, period, env, timeout):
    out = sh(["bash", "flow/run.sh", design, flow, str(period), tag], timeout + 300, TIMEOUT=str(timeout), **env)
    lines = [l for l in out.splitlines() if l.startswith("RESULT")]
    f = fields(lines[-1]) if lines else {}
    if "min_period" not in f or "area" not in f:
        return {"ok": False, "why": out[-300:]}
    return {"ok": True, "target": period, "period": float(f["min_period"]), "area": float(f["area"]),
            "tsta": float(f["tsta_min_period"]) if f.get("tsta_min_period") else None, "secs": float(f["synth_s"])}


def stage_stock():
    jobs = [(d, "stock-" + n, "stock", 0, {"SYNTH_OPTS": s, "ABC_OPTS": a.replace(
        "@OUT@", "%s/runs/%s/stock-%s" % (ROOT, d, n))}, 900)
            for n, (s, a) in stock_configs().items() for d in NAMES]
    parallel("stock", jobs, synth, 7)


def stage_speed():
    jobs = [(d, "fast", "ta", 0, {}, 3000) for d in NAMES]
    jobs += [(d, "fast-e0", "ta", 0, {"TA_OPTS": "-effort 0"}, 3000) for d in NAMES]
    parallel("speed", jobs, synth, 5)


def stage_sweep():
    speed = load("speed")
    jobs = [(d, "x%s" % f, "ta", round(speed[d, "fast"]["period"] * f, 3), {}, 3600)
            for f in FACTORS for d in NAMES if speed.get((d, "fast"), {}).get("ok")]
    parallel("sweep", jobs, synth, 6)


def ok_runs(*stages):
    return [(d, t) for s in stages for (d, t), r in load(s).items() if r["ok"]]


def limits(design, tag):
    _, top, clk = next(d for d in DESIGNS if d[0] == design)[:3]
    out = sh(["sta", "-no_splash", "-exit", "flow/limits_eval.tcl"], 300,
             LIB=lib(), NET="runs/%s/%s/net.v" % (design, tag), TOP=top, CLK=clk)
    m = re.search(r"SLEW (\d+) CAP (\d+)", out)
    return {"ok": True, "slew": int(m.group(1)), "cap": int(m.group(2))} if m else {"ok": False, "why": out[-300:]}


def lib():
    return os.environ.get("LIB") or os.path.join(
        os.environ["PDKPATH"], "libs.ref/sky130_fd_sc_hd/lib/sky130_fd_sc_hd__tt_025C_1v80.lib")


def stage_limits():
    parallel("limits", ok_runs("stock", "speed", "sweep"), limits, 6)


def verify(design, tag):
    net = "runs/%s/%s/net.v" % (design, tag)
    out = sh(["bash", "verify/formal.sh", design, net], 1500, FTIME="300")
    m = re.search(r"FORMAL .*status=(\S+)(.*)", out)
    res = {"ok": True, "formal": m.group(1) if m else "ERROR", "detail": (m.group(2).strip() if m else out[-200:])}
    if tag == "fast":
        res["sim"] = []
        for seed in ("1", "2", "3"):
            out = sh(["bash", "verify/check.sh", design, net, "3000", seed], 900)
            m = re.search(r"VERIFY .*status=(\S+)", out)
            res["sim"].append(m.group(1) if m else "ERROR")
    return res


def stage_verify():
    parallel("verify", ok_runs("speed", "sweep"), verify, 6)


def pnr(design, tag, period):
    out = sh(["bash", "pnr/pnr_run.sh", design, tag, str(period)], 1900, THREADS="2", TIMEOUT="1800")
    res = {"ok": False, "why": out[-300:]}
    for line in out.splitlines():
        f = fields(line)
        if line.startswith("PNR_RESULT") and "min_period" in f:
            if res["ok"] is False:
                res = {"ok": False}
            res[f["stage"]] = float(f["min_period"])
            if f["stage"] == "repaired":
                res.update(ok=True, area=float(f["area"]), target=period)
    return res


def stage_pnr():
    speed = load("speed")
    # one target per design that no netlist reaches, so that the repair works equally hard on all of them
    target = {d: round(0.8 * speed[d, "fast"]["period"], 3) for d in NAMES if speed.get((d, "fast"), {}).get("ok")}
    jobs = [(d, t, target[d]) for d, t in ok_runs("stock", "speed") if d in target]
    parallel("pnr", jobs, pnr, 4)


def gmean(v):
    v = list(v)
    return math.exp(sum(map(math.log, v)) / len(v)) if v else float("nan")


def table(head, rows):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] +
                     ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]) + "\n"


def compare(stock, ta, key, area="area"):
    """per design: best stock configuration against the timing-aware netlist"""
    rows, ratio = [], []
    for d in NAMES:
        cand = [r for (dn, t), r in stock.items() if dn == d and t.startswith("stock-") and r["ok"]]
        t = ta.get((d, "fast"))
        if not cand or not t or not t["ok"]:
            rows.append([d] + ["n/a"] * 7)
            continue
        s = min(cand, key=lambda r: r[key])
        sp, ar = s[key] / t[key], t[area] / s[area]
        ratio.append((d, sp, ar))
        rows.append([d + (" (held out)" if d in HOLDOUT else ""), s["tag"][6:], "%.1f" % (1e3 / s[key]), "%.1f" % (1e3 / t[key]),
                     "**%.2fx**" % sp, "%.0f" % s[area], "%.0f" % t[area], "%+.0f%%" % (100 * (ar - 1))])
    print(table(["design", "best stock configuration", "stock fmax [MHz]", "synth_timing fmax [MHz]", "fmax gain",
                 "stock area", "synth_timing area", "area change"], rows))
    for name, sel in (("all", ratio), ("held out", [r for r in ratio if r[0] in HOLDOUT])):
        if sel:
            print("geomean %s (%d): fmax %.3fx, area %.3fx; faster on %d, slower on %d; range %.2f..%.2f\n" % (
                name, len(sel), gmean(r[1] for r in sel), gmean(r[2] for r in sel), sum(r[1] > 1 for r in sel),
                sum(r[1] < 1 for r in sel), min(r[1] for r in sel), max(r[1] for r in sel)))


def stage_report():
    stock, speed, sweep = load("stock"), load("speed"), load("sweep")
    lim, ver, place = load("limits"), load("verify"), load("pnr")

    print("## pre-layout\n")
    compare(stock, speed, "period")
    print("## after placement and repair\n")
    compare(place, place, "repaired")

    print("## stock configurations (geomean fmax of synth_timing relative to each)\n")
    rows = []
    for tag in sorted({t for _, t in stock}):
        for label, data, key in (("pre-layout", stock, "period"), ("placed", place, "repaired")):
            ta = speed if label == "pre-layout" else place
            r = [data[d, tag][key] / ta[d, "fast"][key] for d in NAMES
                 if data.get((d, tag), {}).get("ok") and ta.get((d, "fast"), {}).get("ok")]
            rows.append([tag[6:], label, len(r), "%.2fx" % gmean(r)])
    print(table(["configuration", "", "designs", "gain"], rows))

    print("## effort 0 against default effort\n")
    both = [(speed[d, "fast-e0"], speed[d, "fast"]) for d in NAMES
            if speed.get((d, "fast-e0"), {}).get("ok") and speed.get((d, "fast"), {}).get("ok")]
    print("fmax %.3fx, area %.3fx, runtime %.2fx\n" % (gmean(b["period"] / a["period"] for a, b in both),
          gmean(a["area"] / b["area"] for a, b in both), gmean(a["secs"] / b["secs"] for a, b in both)))

    print("## area for a clock period\n")
    rows, met, total, versus, only = [], 0, 0, [], 0
    for d in NAMES:
        row = [d]
        fast = speed.get((d, "fast"), {})
        for f in FACTORS:
            r = sweep.get((d, "x%s" % f), {})
            if not r.get("ok"):
                row.append("fail")
                continue
            total += 1
            hit = r["period"] <= r["target"] + 5e-4
            met += hit
            row.append("%+.0f%%%s" % (100 * (r["area"] / fast["area"] - 1), "" if hit else " MISSED"))
            if hit:
                meet = [s["area"] for (dn, _), s in stock.items() if dn == d and s["ok"] and s["period"] <= r["target"] + 5e-4]
                if meet:
                    versus.append(r["area"] / min(meet))
                else:
                    only += 1
        rows.append(row)
    print(table(["design"] + ["T = %sx fastest" % f for f in FACTORS], rows))
    print("targets met %d/%d; met by no stock configuration %d; where stock also meets: %d, area geomean %.3fx, smaller in %d\n" % (
        met, total, only, len(versus), gmean(versus), sum(v < 1 for v in versus)))

    print("## limits\n")
    for name, sel in (("stock", lambda t: t.startswith("stock-")), ("synth_timing", lambda t: not t.startswith("stock-"))):
        r = [x for (_, t), x in lim.items() if sel(t) and x["ok"]]
        print("%s: %d netlists, %d with violations, %d slew, %d cap" % (
            name, len(r), sum(x["slew"] + x["cap"] > 0 for x in r), sum(x["slew"] for x in r), sum(x["cap"] for x in r)))

    print("\n## tsta against OpenSTA\n")
    diff = sorted((abs(r["tsta"] - r["period"]), d, t) for data in (stock, speed, sweep)
                  for (d, t), r in data.items() if r["ok"] and r["tsta"] is not None)
    if diff:
        print("%d netlists, largest differences: %s" % (len(diff), diff[-5:]))

    print("\n## equivalence\n")
    count = {}
    for (d, t), r in ver.items():
        count.setdefault(r["formal"], []).append("%s/%s" % (d, t))
    for k, v in count.items():
        print("formal %s: %d %s" % (k, len(v), "" if k == "PROVEN" else v))
    print("simulation:", {d: r["sim"] for (d, t), r in ver.items() if "sim" in r})

    print("\n## failures and runtime\n")
    for name, data in (("stock", stock), ("speed", speed), ("sweep", sweep), ("limits", lim), ("pnr", place)):
        bad = [k for k, r in data.items() if not r["ok"]]
        print("%s: %d runs, %d failed %s" % (name, len(data), len(bad), bad))
    print("synth seconds:", {d: speed[d, "fast"]["secs"] for d in NAMES if speed.get((d, "fast"), {}).get("ok")})


if __name__ == "__main__":
    os.makedirs(RES, exist_ok=True)
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for stage in sys.argv[1:]:
        globals()["stage_" + stage]()
