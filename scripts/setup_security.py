#!/usr/bin/env python3
"""Provision a new private simulation. Passwords are prompted, never command arguments."""
import argparse
import getpass
import hashlib
import os
from pathlib import Path
import secrets


def provision(state, passwords):
    for password in passwords.values():
        if not 8 <= len(password.encode()) <= 128 or '\n' in password or '\x00' in password:
            raise ValueError('Passwords must be 8-128 bytes without newlines or NUL')
    state = Path(state).resolve()
    state.mkdir(mode=0o700, parents=True, exist_ok=False)
    root = state / 'workspace'
    root.mkdir()
    for path in ['system', 'shared', 'home', 'home/admin', 'home/student', 'home/guest']:
        (root/path).mkdir()
    (root/'system/config.txt').write_text('system configuration: protected\n')
    (root/'shared/events.txt').write_text('error zebra\ninfo ready\nerror alpha\n')
    (root/'shared/readme.txt').write_text('Shared shell demonstration\n')
    (root/'shared/task.txt').write_text('Simulated executable resource\n')
    users=[]
    for name, role in [('admin','admin'),('student','standard'),('guest','standard')]:
        password=passwords[name]
        if not 8 <= len(password.encode()) <= 128 or '\n' in password:
            raise ValueError('Passwords must be 8-128 bytes without newlines')
        salt=secrets.token_bytes(16)
        digest=hashlib.pbkdf2_hmac('sha256',password.encode(),salt,600000).hex()
        users.append(f'{name} {role} {salt.hex()} {digest}\n')
    (state/'users.db').write_text(''.join(users))
    os.chmod(state/'users.db',0o600)
    entries=[('.', 'admin', '755'),('system','admin','755'),('system/config.txt','admin','644'),
             ('shared','admin','777'),('shared/events.txt','admin','644'),('shared/readme.txt','admin','644'),
             ('shared/task.txt','admin','744'),('home','admin','755')]
    entries += [(f'home/{name}',name,'700') for name in passwords]
    import json
    (state/'acl.db').write_text(''.join(f'{json.dumps(path)} {json.dumps(owner)} {mode}\n' for path,owner,mode in entries))
    os.chmod(state/'acl.db',0o600)
    return state

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state',default='.osh-security')
    options=parser.parse_args()
    values={}
    for name in ['admin','student','guest']:
        password=getpass.getpass(f'Choose {name} password: ')
        if password != getpass.getpass('Confirm password: '):
            raise SystemExit('Passwords do not match')
        values[name]=password
    path=provision(options.state,values)
    print(f'Created {path}. Start: ./build/os-shell --state {path}')
