#!/usr/bin/env python3
"""Independent paging oracles and actual-process producer-consumer checks."""
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import unittest

from support import STATE, AUTH

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('OS_SHELL_BINARY', ROOT / 'build/os-shell')).resolve()

def run(commands, timeout=10):
    return subprocess.run([str(BINARY),"--state",str(STATE)], input=AUTH+'\n'.join(commands)+'\n', text=True,
                          capture_output=True, timeout=timeout, check=False)

def fields(line):
    return dict(re.findall(r'([a-z_]+)=([^ ]+)', line.strip()))

def record(output, prefix):
    return fields(next(line for line in output.splitlines() if line.startswith(prefix)))

def model(trace, capacity, lru):
    resident=[]
    faults=0
    for page in trace:
        if page in resident:
            if lru:
                resident.remove(page)
                resident.append(page)
        else:
            faults+=1
            if len(resident)==capacity: resident.pop(0)
            resident.append(page)
    return faults, set(resident)

class PagingTests(unittest.TestCase):
    def test_canonical_fifo_lru(self):
        for policy,expected in [('fifo',7),('lru',5)]:
            result=run([f'memory demo {policy}', 'exit'])
            self.assertEqual(result.returncode,0,result.stderr)
            stats=record(result.stdout,'MEMORY ')
            self.assertEqual(int(stats['faults']),expected)
            self.assertEqual(int(stats['references']),11)
            self.assertEqual(int(stats['replacements']),expected-3)
            self.assertEqual(int(stats['used_bytes']),12288)

    def test_fifo_does_not_refresh_on_hit(self):
        result=run(['memory init 2 1024 fifo','memory alloc 1 3','memory access 1 0',
                    'memory access 1 1','memory access 1 0','memory access 1 2'])
        self.assertIn('EVICT pid=1 page=0',result.stdout)

    def test_lru_refreshes_on_hit(self):
        result=run(['memory init 2 1024 lru','memory alloc 1 3','memory access 1 0',
                    'memory access 1 1','memory access 1 0','memory access 1 2'])
        self.assertIn('EVICT pid=1 page=1',result.stdout)

    def test_allocations_are_virtual_and_free_returns_frames(self):
        result=run(['memory init 2 4096 fifo','memory alloc 7 10','memory stats',
                    'memory access 7 9','memory access 7 0','memory free 7','memory stats'])
        stats=[fields(x) for x in result.stdout.splitlines() if x.startswith('MEMORY ')]
        self.assertEqual(stats[0]['used_frames'],'0')
        self.assertEqual(stats[1]['used_frames'],'0')
        self.assertIn('logical_bytes=40960 resident_frames=0',result.stdout)
        self.assertIn('FREE pid=7 logical_bytes=40960 released_frames=2',result.stdout)
        self.assertEqual(stats[1]['faults'],'2') # Lifetime fault count survives free.

    def test_equal_page_numbers_are_process_scoped(self):
        result=run(['memory init 2 64 fifo','memory alloc 1 1','memory alloc 2 1',
                    'memory access 1 0','memory access 2 0','memory stats'])
        self.assertEqual(record(result.stdout,'MEMORY ')['faults'],'2')
        self.assertIn('FRAME index=0 pid=1 page=0',result.stdout)
        self.assertIn('FRAME index=1 pid=2 page=0',result.stdout)

    def test_cross_process_eviction_updates_residency(self):
        result=run(['memory init 1 100 lru','memory alloc 1 1','memory alloc 2 1',
                    'memory access 1 0','memory access 2 0','memory stats'])
        self.assertIn('EVICT pid=1 page=0',result.stdout)
        proc=[fields(x) for x in result.stdout.splitlines() if x.startswith('PROCESS ')]
        self.assertEqual([p['resident_bytes'] for p in proc],['0','100'])

    def test_release_reuse_is_a_fault(self):
        result=run(['memory init 1 100 fifo','memory alloc 1 1','memory access 1 0',
                    'memory free 1','memory alloc 1 1','memory access 1 0','memory stats'])
        self.assertEqual(record(result.stdout,'MEMORY ')['faults'],'2')
        self.assertNotIn('EVICT',result.stdout)

    def test_reset_clears_accounting(self):
        result=run(['memory demo fifo','memory init 1 32 lru','memory stats'])
        stats=[fields(x) for x in result.stdout.splitlines() if x.startswith('MEMORY ')]
        self.assertEqual(stats[-1]['faults'],'0')
        self.assertEqual(stats[-1]['used_bytes'],'0')

    def test_invalid_input_does_not_change_state(self):
        commands=['memory stats','memory init 0 4096 fifo','memory init 1 0 fifo',
                  'memory init 1 16 other','memory init 1 16 fifo','memory alloc 1 2',
                  'memory alloc 1 9','memory access 1 2','memory access 2 0',
                  'memory access 1 -1','memory free 2','memory alloc x 2',
                  'memory init 999999999999999999999999 16 fifo','memory stats','exit 0']
        result=run(commands)
        self.assertEqual(result.returncode,0)
        stats=record(result.stdout,'MEMORY ')
        self.assertEqual(stats['references'],'0')
        self.assertEqual(stats['faults'],'0')
        self.assertIn('pages=2 logical_bytes=32',result.stdout)
        self.assertGreaterEqual(result.stderr.count('os-shell: memory:'),10)

    def test_background_simulations_rejected(self):
        result=run(['memory init 1 16 fifo &','sync pc 1 1 1 1 &','exit 0'])
        self.assertEqual(result.stderr.count("stateful commands require a standalone foreground command"),2)
        self.assertNotIn('SYNC_RESULT',result.stdout)

    def test_randomized_reference_model_and_belady(self):
        rng=random.Random(630)
        random_trace=[rng.randrange(6) for _ in range(100)]
        belady=[1,2,3,4,1,2,5,1,2,3,4,5]
        for trace in [random_trace,belady]:
            for capacity in [1,3,4]:
                for policy in ['fifo','lru']:
                    commands=[f'memory init {capacity} 256 {policy}','memory alloc 1 6']
                    commands += [f'memory access 1 {p}' for p in trace]
                    result=run(commands+['memory stats'])
                    faults,resident=model(trace,capacity,policy=='lru')
                    self.assertEqual(int(record(result.stdout,'MEMORY ')['faults']),faults)
                    actual={int(fields(x)['page']) for x in result.stdout.splitlines() if x.startswith('FRAME ') and 'FREE' not in x}
                    self.assertEqual(actual,resident)

class SynchronizationTests(unittest.TestCase):
    def verify(self,p,c,items,capacity,delay=0):
        result=run([f'sync pc {p} {c} {items} {capacity} {delay} quiet'])
        self.assertEqual(result.returncode,0,result.stderr)
        stats=record(result.stdout,'SYNC_RESULT ')
        self.assertEqual(stats['verified'],'yes')
        self.assertEqual(int(stats['produced']),p*items)
        self.assertEqual(int(stats['consumed']),p*items)
        self.assertEqual(int(stats['unique']),p*items)
        self.assertEqual(stats['occupancy'],'0')
        self.assertLessEqual(int(stats['max_occupancy']),capacity)
        self.assertEqual(stats['errors'],'0')
        workers=[int(fields(x)['pid']) for x in result.stdout.splitlines() if x.startswith('WORKER ')]
        self.assertEqual(len(workers),p+c)
        self.assertEqual(len(set(workers)),p+c)
        for pid in workers:
            with self.assertRaises(ProcessLookupError): os.kill(pid,0)

    def test_single_slot_many_workers(self): self.verify(4,3,100,1)
    def test_more_consumers_than_items(self): self.verify(1,8,1,1)
    def test_capacity_larger_than_workload(self): self.verify(2,2,2,128)
    def test_stress_exactly_once(self): self.verify(8,8,1000,7)
    def test_small_delayed_run(self): self.verify(2,2,5,2,1)
    def test_repeated_runs_cleanup(self):
        for _ in range(4): self.verify(2,3,10,3)
    def test_trace_shows_locked_process_transactions(self):
        result=run(['sync pc 2 2 3 2'])
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.count('PRODUCE worker_pid='),6)
        self.assertEqual(result.stdout.count('CONSUME worker_pid='),6)
        self.assertEqual(result.stdout.count('mutex=held'),12)
    def test_invalid_sync_arguments(self):
        bad=['sync pc 0 1 2 1','sync pc 1 9 2 1','sync pc 1 1 1 0',
             'sync pc 2 2 6000 4 quiet','sync pc 1 1 201 2','sync pc 1 1 1000 2 20 quiet',
             'sync pc 1 1 1 2 quiet quiet','sync pc 1 1 1 2 1 bad',
             'sync pc 1 1 -1 2','sync pc 1 1 1 2 999999999999999999999']
        result=run(bad+['echo recovery','exit 0'])
        self.assertEqual(result.stderr.count('os-shell: sync:'),len(bad))
        self.assertIn('recovery',result.stdout)
        self.assertNotIn('WORKER',result.stdout)
    def test_worker_failure_reaps_peers_and_recovers(self):
        proc=subprocess.Popen([str(BINARY),"--state",str(STATE)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,text=True,bufsize=1)
        try:
            proc.stdin.write(AUTH+'sync pc 2 2 100 2 20 quiet\necho recovered\nexit 0\n')
            proc.stdin.flush()
            prefix=''
            while True:
                line=proc.stdout.readline()
                prefix+=line
                if line.startswith('WORKER '):
                    os.kill(int(fields(line)['pid']),signal.SIGKILL)
                    break
                self.assertTrue(line,'shell exited before worker creation')
            out,error=proc.communicate(timeout=5)
            self.assertEqual(proc.returncode,0)
            self.assertIn('recovered',out)
            self.assertIn('worker failed or was interrupted',error)
            for pid in re.findall(r'WORKER role=\w+ pid=(\d+)',prefix+out):
                with self.assertRaises(ProcessLookupError): os.kill(int(pid),0)
        finally:
            if proc.poll() is None:
                proc.kill(); proc.communicate()

if __name__=='__main__': unittest.main(verbosity=2)
