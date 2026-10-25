#!/usr/bin/env python3
"""Verify the integrated shell against an independent unit-step scheduling oracle."""
import random
import subprocess
import unittest
from collections import deque
from pathlib import Path
from support import STATE, AUTH
ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/os-shell'


def shell(script):
    return subprocess.run([str(BINARY),"--state",str(STATE)], input=AUTH+script, capture_output=True, text=True, timeout=15)


def metric_rows(output):
    rows = {}
    active = False
    for line in output.splitlines():
        if line.startswith('ID ARRIVAL BURST PRIORITY FIRST COMPLETION WAIT TURNAROUND RESPONSE'):
            active = True
            continue
        if active:
            if line.startswith('AVERAGE'):
                break
            parts = line.split()
            if len(parts) == 9:
                rows[parts[0]] = list(map(int, parts[1:]))
    return rows


def oracle(workload, rr, quantum):
    """One simulated unit per iteration, independent of production event stepping."""
    ready = deque()
    remaining = [p[1] for p in workload]
    first, completion = {}, {}
    now, current, used = 0, None, 0
    while len(completion) < len(workload):
        for i, p in enumerate(workload):
            if p[3] == now:
                ready.append(i)
        if current is not None:
            if remaining[current] == 0:
                completion[current] = now
                current = None
            elif rr and used == quantum:
                ready.append(current)
                current = None
            elif not rr and ready and min(workload[i][2] for i in ready) < workload[current][2]:
                ready.append(current)
                current = None
        if current is None and ready:
            if rr:
                current = ready.popleft()
            else:
                current = min(ready, key=lambda i: (workload[i][2], workload[i][3], i))
                ready.remove(current)
            first.setdefault(current, now)
            used = 0
        if current is not None:
            remaining[current] -= 1
            used += 1
        if len(completion) < len(workload):
            now += 1
    return {p[0]: [p[3], p[1], p[2], first[i], completion[i], completion[i]-p[3]-p[1],
                    completion[i]-p[3], first[i]-p[3]] for i, p in enumerate(workload)}


class SchedulerIntegrationTests(unittest.TestCase):
    def test_random_workloads_against_independent_oracle(self):
        generator = random.Random(630)
        for case in range(40):
            workload = [(f'P{i}', generator.randint(1, 9), generator.randint(0, 4), generator.randint(0, 8))
                        for i in range(generator.randint(1, 7))]
            quantum = generator.randint(1, 5)
            setup = ''.join(f'sched add {name} {burst} {priority} {arrival}\n'
                            for name, burst, priority, arrival in workload)
            for rr in (True, False):
                command = f'sched run rr {quantum} 0' if rr else 'sched run priority 0'
                result = shell(setup + command + '\nexit\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(metric_rows(result.stdout), oracle(workload, rr, quantum), (case, rr, result.stdout))

    def test_invalid_input_and_atomic_workload_update(self):
        result = shell('sched add A 3 1\nsched add A 2 0\nsched add B -1 0\nsched add C 99999999999999999999999999 0\nsched list\nsched run rr 0\nsched run priority 1001\nsched nope\nexit 0\n')
        self.assertEqual(result.returncode, 0)
        self.assertIn('duplicate process ID', result.stderr)
        self.assertEqual(result.stderr.count('os-shell: sched:'), 6)
        self.assertIn('ID BURST PRIORITY ARRIVAL\nA 3 1 0\n', result.stdout)
        self.assertNotIn('Added B', result.stdout)

    def test_reset_and_reruns(self):
        result = shell('sched add A 2 1\nsched run rr 1 0\nsched run priority 0\nsched reset\nsched run priority 0\nexit\n')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout.count('A 0 2 1 0 2 0 2 0'), 2)
        self.assertIn('no processes', result.stderr)

    def test_scheduler_background_rejected(self):
        result = shell('sched add A 1 0 &\nsched list\nexit\n')
        self.assertIn("stateful commands require a standalone foreground command", result.stderr)
        self.assertNotIn('Added A', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
