#!/usr/bin/env python3
"""End-to-end checks for authenticated pipelines and simulated permissions."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from support import provision
from integration_test import PtySession
ROOT=Path(__file__).resolve().parents[1]
BINARY=Path(os.environ.get('OS_SHELL_BINARY',ROOT/'build/os-shell')).resolve()
PASSWORDS={'admin':'AdminTest123!','student':'StudentTest123!','guest':'GuestTest123!'}

class D4Tests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='osh-d4-')
        self.addCleanup(self.temp.cleanup)
        self.state=provision(Path(self.temp.name)/'state',PASSWORDS)
        self.work=self.state/'workspace'
    def run_shell(self,commands,user='admin',prefix=None,timeout=8):
        auth=prefix if prefix is not None else user+'\n'+PASSWORDS[user]+'\n'
        return subprocess.run([str(BINARY),'--state',str(self.state)],cwd=self.work,input=auth+commands,
                              capture_output=True,text=True,timeout=timeout)
    def test_login_required_and_eof(self):
        r=self.run_shell('echo unauthorized\n',prefix='')
        self.assertNotIn('unauthorized',r.stdout)
        self.assertNotEqual(r.returncode,0)
    def test_three_failures_close_session(self):
        r=self.run_shell('echo unauthorized\n',prefix='admin\nwrong\n'*3)
        self.assertEqual(r.stderr.count('Authentication failed'),3)
        self.assertNotIn('unauthorized',r.stdout)
    def test_retry_unknown_user_and_success(self):
        r=self.run_shell('whoami\n',prefix='unknown\nbad\nstudent\nStudentTest123!\n')
        self.assertIn('user=student role=standard',r.stdout)
        self.assertEqual(r.stderr.count('Authentication failed'),1)
    def test_distinct_users(self):
        for user in PASSWORDS:
            r=self.run_shell('whoami\n',user)
            self.assertIn('user='+user,r.stdout)
    def test_passwords_not_stored_in_plaintext(self):
        text=(self.state/'users.db').read_text()
        for password in PASSWORDS.values():self.assertNotIn(password,text)
        records=[x.split() for x in text.splitlines()]
        self.assertEqual(len({x[2] for x in records}),3)
        self.assertEqual((self.state/'users.db').stat().st_mode&0o777,0o600)
    def test_concurrent_sessions_do_not_overwrite_permissions(self):
        import select
        proc=subprocess.Popen([str(BINARY),'--state',str(self.state)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            ready,_,_=select.select([proc.stdout],[],[],3)
            self.assertTrue(ready)
            proc.stdout.read(10) # Username prompt: lock is held before login.
            r=self.run_shell('echo unexpected\n')
            self.assertNotEqual(r.returncode,0)
            self.assertIn('already in use',r.stderr)
        finally:
            proc.communicate(input=b'',timeout=3)

    def test_fail_closed_without_database(self):
        user_db=self.state/'users.db'
        original=user_db.read_text()
        user_db.write_text(original+'truncated-record')
        malformed=self.run_shell('echo unauthorized\n')
        self.assertNotEqual(malformed.returncode,0)
        self.assertNotIn('unauthorized',malformed.stdout)
        user_db.write_text(original)
        (self.state/'acl.db').unlink()
        r=self.run_shell('echo unauthorized\n')
        self.assertNotEqual(r.returncode,0)
        self.assertNotIn('unauthorized',r.stdout)
    def test_multistage_pipeline_and_builtin_source(self):
        r=self.run_shell('ls shared | grep txt\ncat shared/events.txt | grep error | sort\n')
        self.assertIn('events.txt',r.stdout)
        self.assertIn('error alpha\nerror zebra\n',r.stdout)
        self.assertEqual(r.returncode,0,r.stderr)
    def test_large_pipeline_does_not_deadlock(self):
        (self.work/'large.txt').write_bytes(b'abcdef\n'*100000)
        r=self.run_shell('cat large.txt | cat | wc -c\n')
        self.assertRegex(r.stdout,r'\b700000\b')
        self.assertEqual(r.returncode,0,r.stderr)
    def test_early_consumer_and_missing_stages(self):
        r=self.run_shell('/usr/bin/yes | /usr/bin/head -n 1\nno_such_command_xyz | cat\necho recovered\n')
        self.assertIn('recovered',r.stdout)
        self.assertIn('no_such_command_xyz',r.stderr)
    def test_last_stage_status(self):
        self.assertEqual(self.run_shell('echo x | /usr/bin/false\n').returncode,1)
        self.assertEqual(self.run_shell('/usr/bin/false | cat\n').returncode,0)
        self.assertEqual(self.run_shell('echo x | no_such_command_xyz\n').returncode,127)
    def test_pipeline_syntax_and_literal_pipe(self):
        r=self.run_shell('| cat\necho x |\necho x || cat\necho "a|b" | cat\nexit 0\n')
        self.assertEqual(r.stderr.count('empty pipeline stage'),3)
        self.assertIn('a|b\n',r.stdout)
        sixteen='echo boundary'+(' | cat'*15)+'\n'
        self.assertIn('boundary\n',self.run_shell(sixteen).stdout)
        self.assertIn('limit is 16',self.run_shell(sixteen.strip()+' | cat\n').stderr)
    def test_pipeline_background_and_fg(self):
        r=self.run_shell('sleep 0.1 | cat &\njobs\nfg %1\necho complete\n')
        self.assertIn('Running',r.stdout)
        self.assertIn('complete',r.stdout)
    def test_pipeline_state_does_not_change_parent(self):
        r=self.run_shell('cd shared | cat\npwd\n')
        self.assertIn(str(self.work),r.stdout)
    def test_stateful_modules_rejected_in_child_context(self):
        r=self.run_shell('memory init 1 16 fifo | cat\nsched reset &\nsync pc 1 1 1 1 | cat\nchmod 777 shared/task.txt | cat\nexit 0\n')
        self.assertEqual(r.stderr.count('standalone foreground'),4)
        self.assertNotIn('SYNC_RESULT',r.stdout)
    def test_standard_system_read_write_and_remove(self):
        self.run_shell('chmod 777 system/config.txt\n')
        r=self.run_shell('cat ../../system/config.txt\nwrite ../../system/config.txt forbidden\nrm ../../system/config.txt\nchmod 777 ../../system/config.txt\nexit 0\n','student')
        self.assertIn('system configuration: protected',r.stdout)
        self.assertGreaterEqual(r.stderr.count('permission denied'),3)
        self.assertNotIn('forbidden',(self.work/'system/config.txt').read_text())
    def test_admin_write_allowed(self):
        r=self.run_shell('write system/config.txt authorized\ncat system/config.txt\n')
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('authorized',(self.work/'system/config.txt').read_text())
    def test_owner_creation_and_cross_user_denial(self):
        r=self.run_shell('touch private.txt\nwrite private.txt owner-data\nchmod 600 private.txt\ncat private.txt\n','student')
        self.assertEqual(r.returncode,0,r.stderr)
        r=self.run_shell('cat ../student/private.txt\nwrite ../student/private.txt bad\nexit 0\n','guest')
        self.assertEqual(r.stderr.count('permission denied'),2)
    def test_traversal_absolute_and_symlink_escape(self):
        (self.work/'shared/outside').symlink_to('/etc')
        r=self.run_shell('cat /etc/passwd\ncat ../../../../etc/passwd\nls ../../shared/outside\ncd /\nexit 0\n','student')
        self.assertEqual(r.stderr.count('permission denied'),4)
    def test_external_bypass_and_pipeline_authorization(self):
        r=self.run_shell('/bin/cat /etc/passwd\npython3 -c bad\ncat ../../system/config.txt | /bin/sh\ngrep -f /etc/passwd\nsort -o ../../system/config.txt\nexit 0\n','student')
        self.assertEqual(r.stderr.count('permission denied'),5)
        self.assertNotIn('system configuration',r.stdout)
    def test_permission_changes_persist_and_execute_checks(self):
        r=self.run_shell('chmod 604 shared/task.txt\naccess x shared/task.txt\n')
        self.assertEqual(r.returncode,0,r.stderr) # admin override
        r=self.run_shell('access r ../../shared/task.txt\naccess w ../../shared/task.txt\naccess x ../../shared/task.txt\nexit 0\n','student')
        self.assertEqual(r.stdout.count('ALLOW'),1)
        self.assertEqual(r.stdout.count('DENY'),2)
        self.run_shell('chmod 605 shared/task.txt\n')
        r=self.run_shell('access x ../../shared/task.txt\n','student')
        self.assertIn('ALLOW',r.stdout)
        self.run_shell('chown student shared/task.txt\n')
        r=self.run_shell('chmod 700 ../../shared/task.txt\nperm ../../shared/task.txt\n','student')
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('owner=student mode=700',r.stdout)
    def test_standard_pipeline_filters(self):
        r=self.run_shell('cat ../../shared/events.txt | grep error | sort\n','student')
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('error alpha\nerror zebra',r.stdout)
    def test_directory_search_permission(self):
        self.run_shell('chmod 600 shared\n')
        r=self.run_shell('cat ../../shared/events.txt\nexit 0\n','student')
        self.assertIn('permission denied',r.stderr)
        self.run_shell('chmod 000 home/student\n')
        r=self.run_shell('cd\nexit 0\n','student')
        self.assertIn('permission denied',r.stderr)
    def test_modules_integrated_after_pipeline(self):
        r=self.run_shell('echo integrated | cat\nsched add A 2 1\nsched run rr 1 0\nmemory demo lru\nsync pc 2 2 3 2 0 quiet\n')
        self.assertEqual(r.returncode,0,r.stderr)
        for text in ['ALGORITHM RR','faults=5','verified=yes']:self.assertIn(text,r.stdout)

class D4TerminalTests(unittest.TestCase):
    def test_pipeline_stop_resume_interrupt(self):
        s=PtySession()
        try:
            s.expect(rb'\$ ')
            s.send(b'sleep 30 | cat\n')
            import time;time.sleep(.1)
            s.send(b'\x1a');s.expect(rb'\[1\] Stopped.*\$ ')
            s.send(b'bg %1\n');s.expect(rb'\[1\].*\$ ')
            s.send(b'fg %1\n');time.sleep(.1);s.send(b'\x03')
            s.expect(rb'\$ ')
            s.send(b'echo pipeline-recovered\n');s.expect(rb'\r?\npipeline-recovered\r?\n.*\$ ')
        finally:s.close()
    def test_kill_pipeline_job_reaps_all_stages(self):
        import signal
        import time
        def members(group):
            rows=subprocess.check_output(['ps','-axo','pid=,pgid='],text=True).splitlines()
            return [int(pid) for row in rows if len(row.split())==2
                    for pid,pgid in [row.split()] if int(pgid)==group]
        for stopped in [False,True]:
            session=PtySession()
            group=None
            try:
                session.expect(rb'\$ ')
                session.send(b'sleep 30 | cat'+(b'\n' if stopped else b' &\n'))
                if stopped:
                    time.sleep(.1)
                    session.send(b'\x1a')
                    session.expect(rb'\[1\] Stopped.*\$ ')
                    session.send(b'jobs\n')
                    transcript=session.expect(rb'\[1\] Stopped  (\d+).*\$ ')
                    group=int(re.search(rb'\[1\] Stopped  (\d+)',transcript).group(1))
                else:
                    transcript=session.expect(rb'\[1\] (\d+)\r?\n.*\$ ')
                    group=int(re.search(rb'\[1\] (\d+)',transcript).group(1))
                deadline=time.monotonic()+2
                while len(members(group))<3 and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(len(members(group)),3,'coordinator plus two stages')
                session.send(f'kill {group}\n'.encode())
                session.expect(rb'\[1\] Done',timeout=3)
                self.assertEqual(members(group),[],'terminated job must leave no stages behind')
            finally:
                if group is not None and members(group):
                    try:os.killpg(group,signal.SIGKILL)
                    except ProcessLookupError:pass
                session.close()

    def test_password_not_echoed(self):
        # Send credentials immediately on the prompt to exercise echo-disable ordering.
        for _ in range(10):
            s=PtySession()
            try:
                s.expect(rb'\$ ')
                self.assertNotIn(b'AdminTest123!',s.transcript)
            finally:s.close()

if __name__=='__main__':unittest.main(verbosity=2)
