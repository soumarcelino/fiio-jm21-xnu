#!/usr/bin/env python3
"""Ad-hoc sign a thin development Mach-O with explicit XML entitlements."""
import plistlib, struct, sys
from pathlib import Path
from mkinit import code_signature
p=Path(sys.argv[1]); data=bytearray(p.read_bytes()); ent=Path(sys.argv[2]).read_bytes()
plistlib.loads(ent)
if struct.unpack_from('<I',data)[0]!=0xfeedfacf: raise SystemExit('thin Mach-O 64 required')
off=32; sigcmd=None; link=None; text=0
for _ in range(struct.unpack_from('<I',data,16)[0]):
 cmd,size=struct.unpack_from('<II',data,off)
 if cmd==0x1d: sigcmd=off
 if cmd==0x19:
  name=data[off+8:off+24].rstrip(b'\0')
  if name==b'__TEXT': text=struct.unpack_from('<Q',data,off+48)[0]
  if name==b'__LINKEDIT': link=off
 off+=size
if link is None: raise SystemExit('LINKEDIT required')
if sigcmd is None:
 sigcmd=off; start=(len(data)+15)&~15
 if any(data[off:off+16]): raise SystemExit('no load command padding')
 struct.pack_into('<IIII',data,off,0x1d,16,start,0)
 ncmds,cmdsize=struct.unpack_from('<II',data,16)
 struct.pack_into('<II',data,16,ncmds+1,cmdsize+16)
 data.extend(bytes(start-len(data)))
else: start=struct.unpack_from('<I',data,sigcmd+8)[0]
data=data[:start]
sigsize=len(code_signature(data,start,text,ent))
struct.pack_into('<I',data,sigcmd+12,sigsize)
linkoff=struct.unpack_from('<Q',data,link+40)[0]; newsize=start+sigsize-linkoff
struct.pack_into('<Q',data,link+48,newsize)
struct.pack_into('<Q',data,link+32,(newsize+16383)&~16383)
p.write_bytes(data+code_signature(data,start,text,ent))
