# Advanced OS Shell — Deliverable 4

D4 extends the D3 C++17 shell through `Security::authorize/command`, `ParsedCommand::pipeline`, and `executePipeline`. The scheduling, paging, and synchronization modules are retained byte for byte from D3. No GitHub commit or push is performed by the build or packaging scripts.

## Build and provision

macOS needs the Xcode command-line tools and Python 3. Linux additionally needs the OpenSSL development headers/library (`libssl-dev` on Debian/Ubuntu). macOS uses CommonCrypto from its SDK. Linux support is implemented but has not been executed on Linux.

```sh
make
python3 scripts/setup_security.py
./build/os-shell
```

Setup prompts for separate passwords for `admin`, `student`, and `guest`. Use disposable project passwords of 8–128 UTF-8 bytes. Passwords are hidden in a native terminal. Setup refuses to overwrite an existing state. Passwords are not command arguments or shipped defaults. The state is excluded from Git and submission ZIPs.

For another state directory:

```sh
python3 scripts/setup_security.py --state /absolute/path/to/new-state
./build/os-shell --state /absolute/path/to/new-state
```

Log in at `Username:` and `Password:`. Three failures close the session. An unknown username gets the same failure message and password-derivation work. Admin begins in the launch directory; standard users begin in their own simulated home. Exit and launch again to change identity. One session per state is allowed; a host `flock` prevents conflicting permission writes. Use separate states for simultaneous demonstrations.

## Interface

All D1 built-ins and jobs/fg/bg remain available. `cat` now also reads standard input when no filename is given. D2 `sched`, D3 `memory`, and D3 `sync` retain their command interfaces. Enter `sched`, `memory`, or `sync` for usage.

Pipelines support 2–16 stages, quoting/escaped literal pipes, and a final `&`. Examples from the simulated workspace:

```text
ls shared | grep txt
cat shared/events.txt | grep error | sort
echo hello | cat | wc
sleep 10 | cat &
jobs
fg %1
```

Each pipeline is one D1 job with a coordinator and concurrently forked stages in the same process group. Killing a tracked job by its displayed PID signals the whole group, including all pipeline stages; stopped jobs are continued so termination can finish. Exit status is the last stage's status, without `pipefail`. stderr remains on the terminal. Stateful simulation commands, metadata/file mutations, fg, and bg require a standalone foreground command. `cd` or `exit` inside a pipeline affects its child only. Redirection, command lists, expansion, and command history are outside this implementation.

Security built-ins:

```text
whoami
perm PATH
access r PATH
access w PATH
access x PATH
write PATH TEXT
chmod 600 PATH
chown student PATH
```

`write` appends a line to an existing, writable file; use `touch` to create it. `chmod` updates simulated octal permissions, and `chown` requires admin and a known username. Owners can change their own modes; admin can change any workspace entry. Read=4, write=2, execute/search=1. Owner and other bits are enforced; group bits are stored/displayed but there are no simulated groups. `access x` demonstrates executable-resource authorization; arbitrary user programs are not executed for standard users. Directory search is checked along every ancestor. Creation and removal require parent write/search; newly created files/directories default to 600/700 and belong to the logged-in user. Unknown paths have no simulated permissions until registered by a shell creation operation or admin chmod/chown.

`system/` is protected from standard-user writes/removal regardless of mode. The default config is admin-owned 644, so it can be read but not changed by standard users. Standard users are confined to the canonical workspace path, including symlink resolution, and cannot read another user's 700 home. Permissions persist in the private `acl.db` beside the workspace.

Standard external commands are deliberately limited to `grep PATTERN [FILE...]`, `sort [FILE...]`, `wc [FILE...]`, and `sleep SECONDS`. Flags and leading-hyphen arguments are rejected. File operands are checked and binaries execute from fixed `/usr/bin` or `/bin` paths. Standard users cannot call `/bin/sh`, Python, absolute executable paths, or arbitrary programs to bypass simulated permissions. Host-PID `kill` requires admin; standard users can interrupt their foreground jobs with Ctrl+C.

## Simulation boundary

These are application roles running under one host UID. Admin is trusted and retains D1's arbitrary external command execution and host file access. `chmod` does not alter real host mode bits. The host account owner can edit the program/state or race filesystem changes outside the shell. This is an educational authorization simulation, not a sandbox or a production login service. There is no persistent brute-force lockout, password reset, MFA, simulated groups, or protection against a hostile host user. PBKDF2-HMAC-SHA256 uses 600,000 iterations, distinct 16-byte random salts, 32-byte verifiers, and a full-length comparison. Provisioning uses host modes 700/600; startup checks host ownership and rejects group/other access.

## Verification and evidence

```sh
make test
python3 scripts/collect_evidence.py
python3 scripts/package_submission.py
```

The suite has parser and scheduler unit tests plus 71 integration tests (20 D1, 4 D2, 20 D3, 27 D4). Tests provision disposable credentials and authenticate normally. Tests cover terminal password hiding, pipeline job control, large transfers, early consumers, status semantics, failed login, permission persistence, traversal/symlink escapes, filter bypass attempts, exclusive sessions, and integration with prior modules.

Actual execution transcripts and results are in `evidence/`. Evidence session wall times include authentication, process startup, and output capture, and must not be treated as pipeline CPU times. Passwords and hashes are omitted from evidence. `screenshots/CAPTURE_GUIDE.md` gives native screenshot steps. The report builder appends only native images found under the documented filenames; it never generates terminal images.

The APA 7 report is in `report/Advanced_Shell_Deliverable_4_Report.docx`. The report includes all eight original native Terminal screenshots. The report and current ZIP are ready for your final review. `SUBMISSION_CHECKLIST.txt` tracks remaining steps.

To rebuild the Word report, use Python 3.10 or newer and install its optional dependencies in a virtual environment:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r report/requirements.txt
.venv/bin/python report/build_report.py
```
