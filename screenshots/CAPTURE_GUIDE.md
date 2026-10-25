# D4 native Terminal screenshots

Run each command separately and wait for the prompt. Do not paste the whole page. Capture the actual Terminal window with Shift+Command+4, then Space, then click the window. Use the filenames below in this folder. Passwords stay hidden; never include the setup password entry or credential database in a screenshot.

## Setup — host Terminal

```sh
cd /Users/shobhitdhamania/Desktop/Assignments/Advanced_OS_Residency_Deliverable4
make
python3 scripts/setup_security.py
./build/os-shell
```

Skip setup if `.osh-security` already exists. It refuses to overwrite your state. Choose separate disposable passwords for `admin`, `student`, and `guest`.

## 01_admin_login.png — administrator login

At `Username:` enter `admin`, then enter the password you chose. Run:

```text
whoami
cd .osh-security/workspace
perm system/config.txt
write system/config.txt admin-approved-change
cat system/config.txt
```

Take the screenshot. It should show successful admin authentication and the permitted update. Keep the shell open for the next section.

## 02_pipelines.png — piping and a chain

```text
clear
ls shared | grep txt
cat shared/events.txt | grep error | sort
echo integrated-pipeline | cat | wc
```

Take the screenshot. Expect error alpha before error zebra from the sorted three-stage pipeline.

## 03_permissions_admin.png — set read and execute permissions

```text
clear
whoami
chmod 604 shared/task.txt
perm shared/task.txt
access x shared/task.txt
```

Take the screenshot. Admin's access check is ALLOW due to the admin override. The mode is 604; standard users receive only read permission. Then:

```text
exit 0
```

## 04_student_permissions.png — standard login and denied writes

Back in the host Terminal, restart from the repository:

```sh
./build/os-shell
```

Log in as `student` with its password. The starting directory is its simulated home. Run:

```text
whoami
cat ../../system/config.txt
write ../../system/config.txt forbidden-change
rm ../../system/config.txt
access r ../../shared/task.txt
access w ../../shared/task.txt
access x ../../shared/task.txt
```

Capture the screenshot: system file readable, two mutations denied, then ALLOW/DENY/DENY. Keep this session open.

## 05_user_ownership.png — own file and a denied escape

```text
clear
touch personal.txt
write personal.txt student-owned-data
perm personal.txt
cat personal.txt
cat /etc/passwd
cat ../../shared/events.txt | grep error | sort
```

Capture: personal file owner student/mode600, host path denied, approved pipeline works. Then `exit 0`.

Restart `./build/os-shell`, log in as `guest`, and run:

```text
whoami
cat ../student/personal.txt
```

Capture this separately as `06_guest_denial.png`, then `exit 0`.

## 07_execute_allowed.png — grant executable-resource access

Restart as admin, then:

```text
cd .osh-security/workspace
chmod 605 shared/task.txt
perm shared/task.txt
exit 0
```

Restart as student, then:

```text
whoami
access x ../../shared/task.txt
access w ../../shared/task.txt
```

Capture: execute ALLOW and write DENY. This checks the simulation's execute bit; it does not execute an arbitrary host program. Then `exit 0`.

## 08_failed_login.png — rejected authentication

Restart the shell, enter `admin` with an intentionally wrong password three times. Capture the failure messages and return to the host prompt. Do not include any real password.

## Optional 09_integrated_modules.png

Restart as admin. Run separately:

```text
sched reset
sched add A 2 1
sched run rr 1 0
memory demo lru
sync pc 2 2 3 2 1
```

Use a tall Terminal window so results remain readable. One screenshot may not fit; the report already contains the raw integration results.

## Insert and package

After saving original native PNGs into this folder, rebuild the Word report:

```sh
python3 report/build_report.py
python3 scripts/package_submission.py
```

The builder includes the eight required screenshots only when all are present; a partial set produces a clear error. Optional integrated-module images are not automatically inserted. Review the Word pages after rebuilding. The final ZIP must be regenerated after adding screenshots.
