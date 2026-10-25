#!/usr/bin/env python3
"""Package source, report and evidence; exclude credentials and QA artifacts."""
from pathlib import Path
import zipfile
ROOT=Path(__file__).resolve().parents[1]
OUTPUT=ROOT.parent/'Advanced_Shell_Deliverable_4_Submission.zip'
ALLOWED={'.gitignore','Makefile','README.md','SUBMISSION_CHECKLIST.txt'}
TREES={'src','include','tests','scripts','report','evidence','examples','screenshots'}

def package():
    files=[]
    for path in sorted(ROOT.rglob('*')):
        if not path.is_file():continue
        rel=path.relative_to(ROOT)
        if rel.parts[0] not in TREES and str(rel) not in ALLOWED:continue
        if any(p.startswith('.') or p=='__pycache__' or p.startswith('rendered') for p in rel.parts[:-1]):continue
        if path.suffix in {'.pyc','.pdf'} or path.name in {'users.db','acl.db','session.lock'}:continue
        files.append(path)
    with zipfile.ZipFile(OUTPUT,'w',zipfile.ZIP_DEFLATED) as archive:
        for path in files:archive.write(path,ROOT.name+'/'+str(path.relative_to(ROOT)))
    with zipfile.ZipFile(OUTPUT) as archive:
        assert archive.testzip() is None
        for path in files:assert archive.read(ROOT.name+'/'+str(path.relative_to(ROOT)))==path.read_bytes()
    print(f'{OUTPUT}: {len(files)} verified files')
if __name__=='__main__':package()
