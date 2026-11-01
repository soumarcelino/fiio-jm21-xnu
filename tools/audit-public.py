#!/usr/bin/env python3
"""Check the publication file set. Never print matched sensitive values."""
from pathlib import Path
import re
import subprocess
import sys
root = Path(__file__).resolve().parent.parent
files = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=root).decode().split('\0')
patterns = {
    'personal host path': re.compile(rb'(?<![A-Za-z0-9_./-])/(?:home|Users)/[A-Za-z0-9_.-]+/'),
    'private key': re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'access token': re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[A-Z0-9]{16})'),
    'URL credentials': re.compile(rb'https?://[^\s/@:]+:[^\s/@]+@'),
}
errors = []
for name in sorted(set(filter(None, files))):
    p = root / name
    if p.is_symlink():
        errors.append((name, 'symlink')); continue
    data = p.read_bytes()
    if b'\0' in data:
        errors.append((name, 'binary content'))
    for kind, pattern in patterns.items():
        if pattern.search(data): errors.append((name, kind))
for name, kind in errors:
    print(f'{name}: {kind}', file=sys.stderr)
print(f'Publication audit: {len(set(filter(None, files)))} files, {len(errors)} findings')
sys.exit(bool(errors))
