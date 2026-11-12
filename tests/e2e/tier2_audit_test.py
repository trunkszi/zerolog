"""Synthetic gem5 stats and hardware TMA text for the Tier 2 auditor."""

import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.cli.commands.tier2 import (
    _gem5_config,
    audit_samples,
    check_linearity,
    check_memory_order,
    parse_hardware_tma,
    parse_roi,
    tma_quadrants,
)

# Captured from AMDuProfPcm profile -m ipc,pipeline_util -c core=2 on this host.
# Core-2 cycles differ from the System column, and Frontend_Bound.Latency is a child row.
REAL_AMD_CSV = """\
CORE METRICS
Metric,System (Aggregated),Package (Aggregated)-0,CCX (Aggregated)-0,Core-2
Cycles Not in Halt,227770035.00,227770035.00,227770035.00,228059522.42
Frontend_Bound,67.47,67.47,67.47,67.47
Bad_Speculation,1.90,1.90,1.90,1.90
Backend_Bound,24.00,24.00,24.00,24.00
Retiring,10.12,10.12,10.12,10.12
Frontend_Bound.Latency,57.03,57.03,57.03,57.03
Frontend_Bound.BW,10.44,10.44,10.44,10.44
Bad_Speculation.Mispredicts,1.86,1.86,1.86,1.86
Backend_Bound.Memory,23.61,23.61,23.61,23.61
Retiring.Fastpath,9.22,9.22,9.22,9.22
"""

# First block of each capture is the ROI dump (memOrderViolationEvents 0, idleCycles omitted).
# The second block is the process-exit dump from the same stats.txt (16 violations, idleCycles printed).
REAL_GEM5 = {
    512: """\
---------- Begin Simulation Statistics ----------
system.cpu.numCycles                            68255                       # Number of cpu cycles simulated (Cycle)
system.cpu.commitStats0.numInsts               151587                       # Number of instructions committed (thread level) (Count)
system.cpu.commitStats0.numOps                 239168                       # Number of ops (including micro ops) committed (thread level) (Count)
system.cpu.iew.memOrderViolationEvents              0                       # Number of memory order violations (Count)
system.cpu.rename.status::Idle                  21005     30.77%     99.79% # Number of cycles rename is idle (Cycle)
system.cpu.rename.status::Squashing                 2      0.00%     99.80% # Number of cycles rename is squashing (Cycle)
system.cpu.rename.renamedInsts                 239169                       # Number of instructions processed by rename (Count)
---------- End Simulation Statistics   ----------
---------- Begin Simulation Statistics ----------
system.cpu.numCycles                           125617                       # Number of cpu cycles simulated (Cycle)
system.cpu.idleCycles                           21444                       # Total number of cycles that the CPU has spent unscheduled due to idling (Cycle)
system.cpu.commitStats0.numInsts               169867                       # Number of instructions committed (thread level) (Count)
system.cpu.commitStats0.numOps                 268752                       # Number of ops (including micro ops) committed (thread level) (Count)
system.cpu.iew.memOrderViolationEvents             16                       # Number of memory order violations (Count)
system.cpu.rename.status::Idle                  34471     33.09%     87.66% # Number of cycles rename is idle (Cycle)
system.cpu.rename.status::Squashing               769      0.74%     88.40% # Number of cycles rename is squashing (Cycle)
system.cpu.rename.renamedInsts                 290897                       # Number of instructions processed by rename (Count)
---------- End Simulation Statistics   ----------
""",
    2560: """\
---------- Begin Simulation Statistics ----------
system.cpu.numCycles                           340639                       # Number of cpu cycles simulated (Cycle)
system.cpu.commitStats0.numInsts               757795                       # Number of instructions committed (thread level) (Count)
system.cpu.commitStats0.numOps                1195584                       # Number of ops (including micro ops) committed (thread level) (Count)
system.cpu.iew.memOrderViolationEvents              0                       # Number of memory order violations (Count)
system.cpu.rename.status::Idle                 104967     30.81%     99.96% # Number of cycles rename is idle (Cycle)
system.cpu.rename.status::Squashing                 2      0.00%     99.96% # Number of cycles rename is squashing (Cycle)
system.cpu.rename.renamedInsts                1195616                       # Number of instructions processed by rename (Count)
---------- End Simulation Statistics   ----------
---------- Begin Simulation Statistics ----------
system.cpu.numCycles                           397980                       # Number of cpu cycles simulated (Cycle)
system.cpu.idleCycles                           21431                       # Total number of cycles that the CPU has spent unscheduled due to idling (Cycle)
system.cpu.commitStats0.numInsts               776088                       # Number of instructions committed (thread level) (Count)
system.cpu.commitStats0.numOps                1225185                       # Number of ops (including micro ops) committed (thread level) (Count)
system.cpu.iew.memOrderViolationEvents             16                       # Number of memory order violations (Count)
system.cpu.rename.status::Idle                 118412     31.45%     96.59% # Number of cycles rename is idle (Cycle)
system.cpu.rename.status::Squashing               768      0.20%     96.79% # Number of cycles rename is squashing (Cycle)
system.cpu.rename.renamedInsts                1247380                       # Number of instructions processed by rename (Count)
---------- End Simulation Statistics   ----------
""",
    4608: """\
---------- Begin Simulation Statistics ----------
system.cpu.numCycles                           613023                       # Number of cpu cycles simulated (Cycle)
system.cpu.commitStats0.numInsts              1364003                       # Number of instructions committed (thread level) (Count)
system.cpu.commitStats0.numOps                2152000                       # Number of ops (including micro ops) committed (thread level) (Count)
system.cpu.iew.memOrderViolationEvents              0                       # Number of memory order violations (Count)
system.cpu.rename.status::Idle                 188935     30.82%     99.98% # Number of cycles rename is idle (Cycle)
system.cpu.rename.status::Squashing                 2      0.00%     99.98% # Number of cycles rename is squashing (Cycle)
system.cpu.rename.renamedInsts                2152032                       # Number of instructions processed by rename (Count)
---------- End Simulation Statistics   ----------
---------- Begin Simulation Statistics ----------
system.cpu.numCycles                           670256                       # Number of cpu cycles simulated (Cycle)
system.cpu.idleCycles                           21338                       # Total number of cycles that the CPU has spent unscheduled due to idling (Cycle)
system.cpu.commitStats0.numInsts              1382296                       # Number of instructions committed (thread level) (Count)
system.cpu.commitStats0.numOps                2181601                       # Number of ops (including micro ops) committed (thread level) (Count)
system.cpu.iew.memOrderViolationEvents             16                       # Number of memory order violations (Count)
system.cpu.rename.status::Idle                 202357     31.18%     98.02% # Number of cycles rename is idle (Cycle)
system.cpu.rename.status::Squashing               769      0.12%     98.14% # Number of cycles rename is squashing (Cycle)
system.cpu.rename.renamedInsts                2203791                       # Number of instructions processed by rename (Count)
---------- End Simulation Statistics   ----------
""",
}

BASE = {
    "num_cycles": 1000.0,
    "idle_cycles": 10.0,
    "committed_insts": 800.0,
    "committed_ops": 900.0,
    "renamed_insts": 910.0,
    "rename_idle_cycles": 5.0,
    "rename_squash_cycles": 1.0,
    "mem_order_violations": 0.0,
}


def block(counters, prefix="system.cpu.", banner=True):
    keys = {
        "num_cycles": "numCycles",
        "idle_cycles": "idleCycles",
        "committed_insts": "commitStats0.numInsts",
        "committed_ops": "commitStats0.numOps",
        "renamed_insts": "rename.renamedInsts",
        "rename_idle_cycles": "rename.idleCycles",
        "rename_squash_cycles": "rename.squashCycles",
        "mem_order_violations": "iew.memOrderViolationEvents",
    }
    lines = ["---------- Begin Simulation Statistics ----------"] if banner else []
    for name, stat in keys.items():
        lines.append(f"{prefix}{stat} {counters[name]}")
    if banner:
        lines.append("---------- End Simulation Statistics   ----------")
    return "\n".join(lines) + "\n"


def scaled(cycles, ops, violations=0.0):
    sample = dict(BASE)
    sample["num_cycles"] = cycles
    sample["committed_ops"] = ops
    sample["committed_insts"] = ops
    sample["renamed_insts"] = ops
    sample["mem_order_violations"] = violations
    return sample


def passing_samples():
    return [
        (512, scaled(1000, 900)),
        (2560, scaled(3000, 2900)),
        (4608, scaled(5000, 4900)),
    ]


class Tier2AuditTest(unittest.TestCase):
    def test_equal_marginal_slopes_pass(self):
        report = audit_samples(passing_samples(), 6)
        self.assertLessEqual(report["linearity"]["drift"], 0.05)
        self.assertEqual(len(report["quadrants"]), 3)

    def test_slope_above_five_percent_is_reported(self):
        samples = [
            (512, scaled(1000, 900)),
            (2560, scaled(3000, 2900)),
            (4608, scaled(9000, 4900)),
        ]
        with self.assertRaisesRegex(RuntimeError, r"marginal cycle slope deviation .* > 0.05"):
            check_linearity(samples)

    def test_memory_order_event_is_reported(self):
        samples = passing_samples()
        samples[1] = (2560, scaled(3000, 2900, violations=1))
        with self.assertRaisesRegex(RuntimeError, r"memOrderViolationEvents=1(\.0)? at n=2560"):
            check_memory_order(samples)

    def test_commit_alias_counts_as_memory_order(self):
        text = block(scaled(1000, 900)).replace("iew.memOrderViolationEvents", "commit.memOrderViolationEvents")
        counters = parse_roi(text)
        self.assertEqual(counters["mem_order_violations"], 0.0)
        text = text.replace("commit.memOrderViolationEvents 0.0", "commit.memOrderViolationEvents 2")
        with self.assertRaisesRegex(RuntimeError, "memOrderViolationEvents=2.0"):
            check_memory_order([(512, parse_roi(text))])

    def test_omitted_idle_cycles_are_zero(self):
        text = block(scaled(1000, 900)).replace("system.cpu.idleCycles 10.0\n", "")
        self.assertEqual(parse_roi(text)["idle_cycles"], 0.0)

    def test_decreasing_counter_is_not_clamped(self):
        samples = passing_samples()
        high = scaled(3000, 2900)
        high["idle_cycles"] = 0
        samples[1] = (2560, high)
        with self.assertRaisesRegex(RuntimeError, r"idle_cycles decreased"):
            check_linearity(samples)

    def test_attributed_sum_above_105_is_reported(self):
        counters = scaled(1000, 10)
        counters["renamed_insts"] = 100000
        counters["rename_squash_cycles"] = 100000
        with self.assertRaisesRegex(RuntimeError, r"TMA attributed sum .* > 105"):
            tma_quadrants(counters, 6)

    def test_first_roi_block_is_the_one_that_counts(self):
        clean = block(scaled(1000, 900))
        dirty = block(scaled(50, 40, violations=4))
        self.assertEqual(parse_roi(clean + dirty)["mem_order_violations"], 0.0)
        with self.assertRaisesRegex(RuntimeError, "memOrderViolationEvents=4.0"):
            check_memory_order([(512, parse_roi(dirty + clean))])

    def test_unbalanced_missing_and_mixed_stats_are_reported(self):
        with self.assertRaisesRegex(RuntimeError, "unbalanced"):
            parse_roi(block(scaled(1000, 900)).replace("End Simulation Statistics", "End Missing"))
        with self.assertRaisesRegex(RuntimeError, "missing counters"):
            parse_roi(block(scaled(1000, 900)).replace("system.cpu.iew.memOrderViolationEvents 0.0\n", ""))
        mixed = block(scaled(1000, 900)).replace(
            "system.cpu.numCycles",
            "board.processor.cores.core.numCycles 5\nsystem.cpu.numCycles",
        )
        with self.assertRaisesRegex(RuntimeError, "mix cpu prefixes"):
            parse_roi(mixed)
        duplicate = block(scaled(1000, 900)).replace(
            "system.cpu.numCycles 1000\n",
            "system.cpu.numCycles 1000\nsystem.cpu.numCycles 1001\n",
            1,
        )
        with self.assertRaisesRegex(RuntimeError, "duplicate gem5 stat"):
            parse_roi(duplicate)

    def test_hardware_text_accepts_four_slots_and_rejects_gaps(self):
        text = "\n".join(
            [
                "cycles: 1200",
                "Retiring: 40.0%",
                "Bad Speculation: 5.0%",
                "Frontend Bound: 20.0%",
                "Backend Bound: 35.0%",
            ]
        )
        report = parse_hardware_tma(text)
        self.assertEqual(report["cycles"], 1200)
        self.assertLessEqual(report["quadrant_sum"], 105)
        with self.assertRaisesRegex(RuntimeError, "cycles=0"):
            parse_hardware_tma(text.replace("cycles: 1200", "cycles: 0"))
        with self.assertRaisesRegex(RuntimeError, "missing slots: backend_bound"):
            parse_hardware_tma(text.replace("Backend Bound: 35.0%", ""))
        with self.assertRaisesRegex(RuntimeError, "quadrant sum"):
            parse_hardware_tma(text.replace("Retiring: 40.0%", "Retiring: 90.0%"))

    def test_negative_backend_residual_is_reported(self):
        counters = scaled(1000, 6120)
        counters["idle_cycles"] = 0
        counters["rename_idle_cycles"] = 0
        counters["rename_squash_cycles"] = 0
        with self.assertRaisesRegex(RuntimeError, r"backend residual .* < -1e-06"):
            tma_quadrants(counters, 6)

    def test_tiny_negative_residual_is_not_clamped(self):
        counters = scaled(1000, 900)
        counters["idle_cycles"] = 0
        counters["rename_idle_cycles"] = 0
        counters["rename_squash_cycles"] = 0
        slots = 6 * 1000
        ops = 100.0000005 * slots / 100.0
        counters["committed_ops"] = ops
        counters["committed_insts"] = ops
        counters["renamed_insts"] = ops
        report = tma_quadrants(counters, 6)
        self.assertLess(report["backend_bound"], 0.0)
        self.assertGreater(report["residual"], -1e-6)

    def test_real_amd_csv_uses_the_pinned_core(self):
        report = parse_hardware_tma(REAL_AMD_CSV, core="2")
        self.assertEqual(report["cycles"], 228059522.42)
        self.assertEqual(report["slots"]["frontend_bound"], 67.47)
        self.assertEqual(report["slots"]["retiring"], 10.12)
        self.assertLessEqual(report["quadrant_sum"], 105)
        with self.assertRaisesRegex(RuntimeError, "column Core-9"):
            parse_hardware_tma(REAL_AMD_CSV, core="9")
        with self.assertRaisesRegex(RuntimeError, "missing slots: frontend_bound"):
            parse_hardware_tma(REAL_AMD_CSV.replace("Frontend_Bound,67.47,67.47,67.47,67.47\n", ""))
        over = REAL_AMD_CSV.replace("Retiring,10.12,10.12,10.12,10.12", "Retiring,10.12,10.12,10.12,80.00")
        with self.assertRaisesRegex(RuntimeError, "quadrant sum"):
            parse_hardware_tma(over, core="2")

    def test_captured_gem5_rois_stay_inside_the_gates(self):
        samples = []
        for n in (512, 2560, 4608):
            counters = parse_roi(REAL_GEM5[n])
            self.assertEqual(counters["mem_order_violations"], 0.0)
            self.assertEqual(counters["idle_cycles"], 0.0)
            samples.append((n, counters))
        self.assertEqual(samples[0][1]["num_cycles"], 68255.0)
        self.assertEqual(samples[1][1]["num_cycles"], 340639.0)
        self.assertEqual(samples[2][1]["num_cycles"], 613023.0)
        report = audit_samples(samples, 6)
        self.assertEqual(report["linearity"]["drift"], 0.0)
        self.assertEqual(report["linearity"]["slopes"], [133.0, 133.0])

    def test_gem5_config_requires_env_var(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop("GEM5_O3_SIM_SCRIPT", None)
            with self.assertRaisesRegex(RuntimeError, "GEM5_O3_SIM_SCRIPT"):
                _gem5_config()
        with patch.dict(os.environ, {"GEM5_O3_SIM_SCRIPT": "/nonexistent/o3_sim.py"}, clear=False):
            with self.assertRaisesRegex(RuntimeError, "GEM5_O3_SIM_SCRIPT"):
                _gem5_config()


if __name__ == "__main__":
    unittest.main()
