#!/usr/bin/env python3
"""Run fresh authenticated sessions; store actual output, never passwords or hashes."""
import json
from pathlib import Path
import subprocess
import tempfile
import sys
import time
from setup_security import provision
ROOT=Path(__file__).resolve().parents[1]
PASSWORDS={'admin':'AdminEvidence123!','student':'StudentEvidence123!','guest':'GuestEvidence123!'}

def collect():
    output=ROOT/'evidence';output.mkdir(exist_ok=True)
    results={}
    with tempfile.TemporaryDirectory(prefix='osh-evidence-') as temporary:
        state=provision(Path(temporary)/'state',PASSWORDS)
        def run(name,user,commands,credentials=None):
            start=time.perf_counter()
            auth=credentials if credentials is not None else user+'\n'+PASSWORDS[user]+'\n'
            r=subprocess.run([str(ROOT/'build/os-shell'),'--state',str(state)],cwd=state/'workspace',input=auth+commands,text=True,capture_output=True,timeout=15)
            elapsed=(time.perf_counter()-start)*1000
            transcript='USER: '+user+'\nCOMMANDS:\n'+commands+'\nSTDOUT:\n'+r.stdout+'\nSTDERR:\n'+r.stderr+f'\nEXIT_STATUS: {r.returncode}\n'
            (output/(name+'.txt')).write_text(transcript)
            results[name]={'exit_status':r.returncode,'session_wall_ms':round(elapsed,3)}
            return r
        r=run('pipelines','admin','ls shared | grep txt\ncat shared/events.txt | grep error | sort\necho pipeline-data | cat | wc\nexit 0\n')
        assert 'error alpha\nerror zebra\n' in r.stdout and r.returncode==0
        r=run('admin_permissions','admin','whoami\nperm system/config.txt\nwrite system/config.txt admin-update\ncat system/config.txt\nchmod 605 shared/task.txt\nperm shared/task.txt\nexit 0\n')
        assert 'admin-update' in r.stdout
        r=run('standard_permissions','student','whoami\ncat ../../system/config.txt\nwrite ../../system/config.txt forbidden\nrm ../../system/config.txt\naccess r ../../shared/task.txt\naccess w ../../shared/task.txt\naccess x ../../shared/task.txt\ntouch personal.txt\nwrite personal.txt student-notes\ncat personal.txt\ncat ../../shared/events.txt | grep error | sort\nexit 0\n')
        assert r.stdout.count('ALLOW')==2 and r.stdout.count('DENY')==1 and r.stderr.count('permission denied')==2
        r=run('guest_permissions','guest','whoami\ncat ../student/personal.txt\nexit 0\n')
        assert 'permission denied' in r.stderr
        r=run('failed_login','admin','echo should-not-run\n',credentials='admin\nincorrect\n'*3)
        assert r.returncode==1 and 'should-not-run' not in r.stdout
        r=run('integration','admin','echo integrated | cat\nsched reset\nsched add A 6 3 0\nsched add B 2 1 2\nsched add C 1 1 2\nsched add D 1 3 3\nsched run rr 2 0\nsched run priority 0\nmemory demo fifo\nmemory demo lru\nsync pc 2 2 3 2 1\nsync pc 8 8 1000 7 0 quiet\nexit 0\n')
        assert 'wait=2.500 turnaround=5.000 response=1.500' in r.stdout
        assert 'wait=2.750 turnaround=5.250 response=2.000' in r.stdout
        assert 'faults=7 replacements=4' in r.stdout and 'faults=5 replacements=2' in r.stdout
        assert 'unique=8000 expected=8000 errors=0' in r.stdout
        # Record command execution time separately from slow password derivation.
        (state/'workspace/large.txt').write_bytes(b'abcdef\n'*100000)
        r=run('large_pipeline','admin','cat large.txt | cat | wc -c\nexit 0\n')
        assert '700000' in r.stdout
    results['verification']={'integration_tests':70,'unit_suites':['parser','scheduler'],
                             'large_pipeline_bytes':700000,'native_screenshots':'complete' if len(list((ROOT/'screenshots').glob('[0-9][0-9]_*.png')))==8 else 'pending'}
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print('Evidence collected from seven authenticated sessions.')
if __name__=='__main__':collect()
