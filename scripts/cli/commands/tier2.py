"""Tier 2 gem5 differential and host TMA checks.

Out-of-range slopes, memory-order events, missing counters, and blocked
hardware tools raise RuntimeError. The CLI prints that error. Nothing here
treats a missing tool or a short sample as a pass.
"""

import csv
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _gem5_config():
    env = os.environ.get("GEM5_O3_SIM_SCRIPT")
    if not env:
        raise RuntimeError("GEM5_O3_SIM_SCRIPT is unset; set GEM5_O3_SIM_SCRIPT to the gem5 o3_sim.py script path")
    config = Path(env)
    if not config.is_file():
        raise RuntimeError(f"gem5 SE config unavailable: {config} (from GEM5_O3_SIM_SCRIPT)")
    return config


GEM5_CANDIDATES = (os.environ.get("UARCH_GEM5_BIN"), "/usr/local/bin/gem5.opt")
POINTS = ((512, "0000000512"), (2560, "0000002560"), (4608, "0000004608"))
PROBE = "encode_u64x2"
CPU = "o3-w6-rob320"
SLOPE_LIMIT = 0.05
QUADRANT_LIMIT = 105.0
RESIDUAL_TOLERANCE = 1e-6
PMU_CORE = "2"
BENCH_ITERATIONS = "5000000"
AMD_BINARY = Path("/usr/local/bin/AMDuProfPcm")
TASKSET_BINARY = Path("/usr/bin/taskset")
AMD_SLOTS = {
    "Retiring": "retiring",
    "Bad_Speculation": "bad_speculation",
    "Frontend_Bound": "frontend_bound",
    "Backend_Bound": "backend_bound",
}
AMD_CYCLES = "Cycles Not in Halt"
PREFIXES = ("system.cpu.", "board.processor.cores.core.", "board.processor.cores0.core.")
ALIASES = {
    "num_cycles": ("numCycles",),
    "idle_cycles": ("idleCycles",),
    "committed_insts": (
        "commitStats0.numInsts",
        "commit.instsCommitted::total",
        "commit.instsCommitted",
        "committedInsts",
    ),
    "committed_ops": (
        "commitStats0.numOps",
        "commit.opsCommitted::total",
        "commit.opsCommitted",
        "committedOps",
    ),
    "renamed_insts": ("rename.renamedInsts", "rename.RenamedInsts"),
    "rename_idle_cycles": ("rename.idleCycles", "rename.IdleCycles", "rename.status::Idle"),
    "rename_squash_cycles": ("rename.squashCycles", "rename.SquashCycles", "rename.status::Squashing"),
    "commit_squashed_insts": ("commit.commitSquashedInsts", "commit.squashedInsts"),
    "mem_order_violations": ("iew.memOrderViolationEvents", "commit.memOrderViolationEvents"),
}
REQUIRED = (
    "num_cycles",
    "idle_cycles",
    "committed_insts",
    "committed_ops",
    "renamed_insts",
    "rename_idle_cycles",
    "rename_squash_cycles",
    "mem_order_violations",
)
# gem5's O3 idleCycles is a formula with prereq(idleCycles), so a zero count is omitted.
ABSENT_MEANS_ZERO = ("idle_cycles",)
BEGIN = "Begin Simulation Statistics"
END = "End Simulation Statistics"


def _number(text):
    try:
        value = float(text)
    except ValueError as error:
        raise RuntimeError(f"gem5 stat is not a number: {text}") from error
    if value < 0 or value != value or value in (float("inf"), float("-inf")):
        raise RuntimeError(f"gem5 stat is negative or non-finite: {text}")
    return value


def stat_blocks(text):
    """Return stats regions. The first complete region is the explicit ROI dump."""
    begins = text.count(BEGIN)
    ends = text.count(END)
    if begins != ends:
        raise RuntimeError(f"gem5 stats markers are unbalanced: begin={begins} end={ends}")
    if begins == 0:
        return [text]
    blocks = []
    cursor = 0
    for _ in range(begins):
        start = text.find(BEGIN, cursor)
        finish = text.find(END, start)
        if start < 0 or finish < 0:
            raise RuntimeError(f"gem5 stats markers disappeared while splitting begin={begins} end={ends}")
        blocks.append(text[start:finish])
        cursor = finish + len(END)
    return blocks


def parse_gem5_block(text):
    raw = {}
    prefix = ""
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        key, value = parts[0], parts[1]
        matched = next((item for item in PREFIXES if key.startswith(item)), None)
        if key.endswith(".numCycles") and (matched is None or key != matched + "numCycles"):
            raise RuntimeError(f"gem5 numCycles uses an unexpected cpu prefix: {key}")
        if matched is None:
            continue
        if prefix and prefix != matched:
            raise RuntimeError(f"gem5 stats mix cpu prefixes {prefix} and {matched}")
        prefix = matched
        stat = key[len(matched) :]
        if stat in raw:
            raise RuntimeError(f"duplicate gem5 stat {prefix}{stat}")
        raw[stat] = value
    counters = {}
    for name, aliases in ALIASES.items():
        found = [alias for alias in aliases if alias in raw]
        if len(found) > 1:
            raise RuntimeError(f"gem5 stat {name} has conflicting aliases: {', '.join(found)}")
        if found:
            counters[name] = _number(raw[found[0]])
    missing = [name for name in REQUIRED if name not in counters and name not in ABSENT_MEANS_ZERO]
    if missing:
        raise RuntimeError("gem5 ROI is missing counters: " + ", ".join(missing))
    for name in ABSENT_MEANS_ZERO:
        counters.setdefault(name, 0.0)
    return counters


def parse_roi(text):
    blocks = stat_blocks(text)
    try:
        counters = parse_gem5_block(blocks[0])
    except RuntimeError as error:
        raise RuntimeError(f"ROI block 1 of {len(blocks)}: {error}") from error
    return counters


def diff_counters(high, low):
    delta = {}
    for name in REQUIRED:
        if high[name] < low[name]:
            raise RuntimeError(f"gem5 counter {name} decreased: {high[name]} < {low[name]}")
        delta[name] = high[name] - low[name]
    if delta["num_cycles"] <= 0 or delta["committed_ops"] <= 0:
        raise RuntimeError(
            f"gem5 marginal sample has no committed work: cycles={delta['num_cycles']} ops={delta['committed_ops']}"
        )
    return delta


def check_linearity(samples, limit=SLOPE_LIMIT):
    """samples is [(n, counters), ...] for the three fixed widths, low to high."""
    if [item[0] for item in samples] != [point for point, _marker in POINTS]:
        raise RuntimeError(f"gem5 samples are not the required widths: {[item[0] for item in samples]}")
    if not limit or limit < 0:
        raise RuntimeError(f"slope limit must be positive: {limit}")
    slopes = []
    for (n0, c0), (n1, c1) in zip(samples, samples[1:]):
        delta = diff_counters(c1, c0)
        slopes.append(delta["num_cycles"] / (n1 - n0))
    drift = abs(slopes[1] - slopes[0]) / slopes[0]
    if drift > limit:
        raise RuntimeError(
            f"marginal cycle slope deviation {drift:.6f} > {limit} (s1={slopes[0]:.6f}, s2={slopes[1]:.6f})"
        )
    return {"slopes": slopes, "drift": drift}


def check_memory_order(samples):
    for n, counters in samples:
        violations = counters["mem_order_violations"]
        if violations != 0:
            raise RuntimeError(f"memOrderViolationEvents={violations} at n={n}")


def tma_quadrants(counters, width):
    if not isinstance(width, int) or width <= 0:
        raise RuntimeError(f"slot width must be a positive integer: {width}")
    active = counters["num_cycles"] - counters["idle_cycles"]
    if active <= 0:
        raise RuntimeError(
            f"TMA active cycles are not positive: numCycles={counters['num_cycles']} idleCycles={counters['idle_cycles']}"
        )
    slots = width * active
    retiring = 100.0 * counters["committed_ops"] / slots
    bad = 100.0 * (
        max(0.0, counters["renamed_insts"] - counters["committed_ops"]) + width * counters["rename_squash_cycles"]
    ) / slots
    frontend = 100.0 * width * counters["rename_idle_cycles"] / slots
    attributed = retiring + bad + frontend
    for name, value in (("retiring", retiring), ("bad_speculation", bad), ("frontend_bound", frontend)):
        if value != value or value in (float("inf"), float("-inf")):
            raise RuntimeError(f"TMA {name} is non-finite")
    if attributed > QUADRANT_LIMIT:
        raise RuntimeError(
            "TMA attributed sum "
            f"{attributed:.4f} > {QUADRANT_LIMIT} "
            f"(retiring={retiring:.4f}, bad_speculation={bad:.4f}, frontend_bound={frontend:.4f})"
        )
    residual = 100.0 - attributed
    if residual < -RESIDUAL_TOLERANCE:
        raise RuntimeError(
            "TMA backend residual "
            f"{residual:.6f} < -{RESIDUAL_TOLERANCE} "
            f"(attributed={attributed:.4f}, retiring={retiring:.4f}, "
            f"bad_speculation={bad:.4f}, frontend_bound={frontend:.4f})"
        )
    backend = residual
    quadrant_sum = attributed + backend
    if quadrant_sum > QUADRANT_LIMIT:
        raise RuntimeError(f"TMA quadrant sum {quadrant_sum:.4f} > {QUADRANT_LIMIT}")
    return {
        "retiring": retiring,
        "bad_speculation": bad,
        "frontend_bound": frontend,
        "backend_bound": backend,
        "attributed": attributed,
        "quadrant_sum": quadrant_sum,
        "residual": residual,
    }


def audit_samples(samples, width):
    check_memory_order(samples)
    linearity = check_linearity(samples)
    quadrants = [tma_quadrants(counters, width) for _n, counters in samples]
    return {"linearity": linearity, "quadrants": quadrants}


def _csv_row(line):
    return next(csv.reader([line]))


def parse_amd_cumulative(text, core=PMU_CORE):
    """Read one Core-N column from an AMDuProfPcm report-cumulative.csv report."""
    lines = text.splitlines()
    header_at = [index for index, line in enumerate(lines) if line.startswith("Metric,")]
    if len(header_at) != 1:
        raise RuntimeError(f"hardware TMA CSV Metric header count is {len(header_at)}")
    header = _csv_row(lines[header_at[0]])
    column_name = f"Core-{core}"
    if header.count(column_name) != 1:
        raise RuntimeError(f"hardware TMA CSV column {column_name} count is {header.count(column_name)}: {header}")
    column = header.index(column_name)
    found = {}
    cycles = None
    for line in lines[header_at[0] + 1 :]:
        if not line.strip():
            break
        row = _csv_row(line)
        if not row:
            continue
        name = row[0].strip()
        if len(row) <= column or row[column].strip() == "":
            raise RuntimeError(f"hardware TMA CSV row {name} has no {column_name} value")
        if name == AMD_CYCLES:
            value = _number(row[column])
            if cycles is not None and cycles != value:
                raise RuntimeError(f"hardware cycles disagree: {cycles} vs {value}")
            cycles = value
        elif name in AMD_SLOTS:
            key = AMD_SLOTS[name]
            value = _number(row[column])
            if key in found and found[key] != value:
                raise RuntimeError(f"hardware TMA {key} disagrees: {found[key]} vs {value}")
            found[key] = value
    if cycles is None:
        raise RuntimeError("hardware TMA CSV has no Cycles Not in Halt\n" + text[-1500:])
    if cycles <= 0:
        raise RuntimeError(f"hardware cycles={cycles}")
    missing = [name for name in AMD_SLOTS.values() if name not in found]
    if missing:
        raise RuntimeError("hardware TMA output is missing slots: " + ", ".join(missing) + "\n" + text[-1500:])
    total = sum(found[name] for name in AMD_SLOTS.values())
    if total > QUADRANT_LIMIT:
        raise RuntimeError(f"hardware TMA quadrant sum {total:.4f} > {QUADRANT_LIMIT}: {found}")
    return {"cycles": cycles, "slots": found, "quadrant_sum": total, "percent": True}


def parse_hardware_tma(text, core=PMU_CORE):
    if "Cycles Not in Halt" in text and any(line.startswith("Metric,") for line in text.splitlines()):
        return parse_amd_cumulative(text, core)
    import re

    slots = {
        "retiring": r"retiring(?:[\s_-]*slots?)?",
        "bad_speculation": r"bad[\s_-]*speculation(?:[\s_-]*slots?)?",
        "frontend_bound": r"(?:frontend[\s_-]*bound|front[\s_-]*end[\s_-]*bound)(?:[\s_-]*slots?)?",
        "backend_bound": r"(?:backend[\s_-]*bound|back[\s_-]*end[\s_-]*bound)(?:[\s_-]*slots?)?",
    }
    found = {}
    for name, pattern in slots.items():
        matches = re.findall(rf"{pattern}\s*[:=]\s*([0-9]+(?:\.[0-9]+)?)\s*%?", text, re.I)
        if not matches:
            continue
        values = [_number(item) for item in matches]
        if any(item != values[0] for item in values):
            raise RuntimeError(f"hardware TMA {name} disagrees across samples: {values}")
        found[name] = values[0]
    missing = [name for name in slots if name not in found]
    cycles = re.findall(r"(?:^|\n)\s*(?:cpu-)?cycles\s*[:=]\s*([0-9]+)\b", text, re.I)
    if not cycles:
        cycles = re.findall(r"\b([0-9][0-9,]*)\s+(?:cpu-)?cycles\b", text, re.I)
    if not cycles:
        raise RuntimeError("hardware TMA output has no cycles counter\n" + text[-1500:])
    parsed_cycles = []
    for item in cycles:
        parsed_cycles.append(_number(item.replace(",", "")))
    if any(item != parsed_cycles[0] for item in parsed_cycles):
        raise RuntimeError(f"hardware cycles disagree across samples: {parsed_cycles}")
    if parsed_cycles[0] <= 0:
        raise RuntimeError(f"hardware cycles={parsed_cycles[0]}")
    if missing:
        raise RuntimeError("hardware TMA output is missing slots: " + ", ".join(missing) + "\n" + text[-1500:])
    total = sum(found.values())
    if total > QUADRANT_LIMIT:
        raise RuntimeError(f"hardware TMA quadrant sum {total:.4f} > {QUADRANT_LIMIT}: {found}")
    return {"cycles": parsed_cycles[0], "slots": found, "quadrant_sum": total, "percent": all(item <= 100 for item in found.values())}


def _executable(path):
    return bool(path) and Path(path).is_file() and os.access(path, os.X_OK)


def gem5_binary():
    for candidate in (*GEM5_CANDIDATES, shutil.which("gem5.opt")):
        if _executable(candidate):
            return Path(candidate)
    raise RuntimeError("gem5.opt unavailable: checked UARCH_GEM5_BIN, /usr/local/bin/gem5.opt, and PATH")


def slot_width(cpu=CPU):
    gem5_config = _gem5_config()
    result = subprocess.run(
        [os.environ.get("PYTHON", "python3"), str(gem5_config), "--dump-profile-json", cpu],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gem5 profile query failed for {cpu}: exit {result.returncode}\n{result.stdout}{result.stderr}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"gem5 profile query returned non-JSON for {cpu}: {result.stdout}") from error
    width = payload.get("slot_width")
    if not isinstance(width, int) or width <= 0:
        raise RuntimeError(f"gem5 profile {cpu} has no positive slot_width: {result.stdout}")
    return width


def _run(command, env=None, timeout=180):
    prefix = [shutil.which("timeout"), str(timeout)] if shutil.which("timeout") else []
    result = subprocess.run(prefix + [str(item) for item in command], text=True, capture_output=True, env=env, check=False)
    return result


def run_gem5_point(binary, marker, outdir):
    gem5 = gem5_binary()
    gem5_config = _gem5_config()
    env = os.environ.copy()
    env["GLIBC_TUNABLES"] = "glibc.pthread.rseq=0"
    command = [
        gem5,
        f"--outdir={outdir}",
        gem5_config,
        "--cpu",
        CPU,
        "--binary",
        binary,
        "--probe",
        PROBE,
        "--iterations",
        marker,
    ]
    result = _run(command, env=env, timeout=600)
    simout = (Path(outdir) / "simout").read_text(errors="replace") if (Path(outdir) / "simout").is_file() else ""
    combined = result.stdout + result.stderr + simout
    if result.returncode != 0:
        raise RuntimeError(
            f"gem5 failed for n={marker}: {gem5} exit {result.returncode}\n{combined[-4000:]}"
        )
    if marker not in combined or "roi=gem5" not in combined:
        raise RuntimeError(f"gem5 guest did not report marker {marker} inside an ROI\n{combined[-4000:]}")
    stats = Path(outdir) / "stats.txt"
    if not stats.is_file():
        raise RuntimeError(f"gem5 produced no stats.txt for n={marker}: {outdir}")
    return stats.read_text(errors="replace")


def _cumulative_csv(outdir):
    found = sorted(Path(outdir).rglob("report-cumulative.csv"))
    if len(found) != 1:
        names = ", ".join(str(path) for path in found)
        raise RuntimeError(f"AMDuProfPcm produced {len(found)} report-cumulative.csv files under {outdir}: {names}")
    return found[0]


def collect_hardware_tma(binary):
    if not _executable(AMD_BINARY):
        raise RuntimeError(f"hardware TMA tool unavailable: {AMD_BINARY} is not executable")
    if not _executable(TASKSET_BINARY):
        raise RuntimeError(f"hardware TMA tool unavailable: {TASKSET_BINARY} is not executable")
    with tempfile.TemporaryDirectory(prefix="zerolog-amd-") as temporary:
        command = [
            AMD_BINARY,
            "profile",
            "-m",
            "ipc,pipeline_util",
            "-c",
            f"core={PMU_CORE}",
            "-O",
            temporary,
            "--",
            TASKSET_BINARY,
            "-c",
            PMU_CORE,
            binary,
            "--probe",
            PROBE,
            "--bench-iterations",
            BENCH_ITERATIONS,
        ]
        result = _run(command, timeout=180)
        text = result.stdout + "\n" + result.stderr
        if result.returncode != 0:
            raise RuntimeError(f"{AMD_BINARY} exit {result.returncode}\n{text[-4000:]}")
        if "ZEROLOG_GEM5" not in text or BENCH_ITERATIONS not in text:
            raise RuntimeError(f"AMDuProfPcm guest did not report bench iterations {BENCH_ITERATIONS}\n{text[-4000:]}")
        try:
            csv_path = _cumulative_csv(temporary)
            return parse_hardware_tma(csv_path.read_text(errors="replace"), core=PMU_CORE)
        except RuntimeError as error:
            raise RuntimeError(f"{AMD_BINARY} exit 0 but counters were rejected: {error}\n{text[-2000:]}") from error


def execute(args):
    from .core import run

    build = Path(args.build_dir) if args.build_dir else ROOT / "build" / args.preset
    (build / "tmp").mkdir(parents=True, exist_ok=True)
    print(run(["cmake", "--preset", args.preset, "-S", ROOT, "-B", build]))
    print(run(["cmake", "--build", build, "--parallel", str(args.jobs)]))
    print(run(["ctest", "--test-dir", build, "--output-on-failure"]))
    binary = build / "zerolog_gem5_harness"
    if not binary.is_file():
        raise RuntimeError(f"zerolog_gem5_harness is not in {build}")
    width = slot_width()
    samples = []
    for n, marker in POINTS:
        with tempfile.TemporaryDirectory(prefix=f"zerolog-gem5-{marker}-") as temporary:
            samples.append((n, parse_roi(run_gem5_point(binary, marker, temporary))))
    report = audit_samples(samples, width)
    hardware = collect_hardware_tma(binary)
    print(
        json.dumps(
            {
                "tier": 2,
                "cpu": CPU,
                "slot_width": width,
                "drift": report["linearity"]["drift"],
                "hardware_cycles": hardware["cycles"],
            },
            sort_keys=True,
        )
    )
