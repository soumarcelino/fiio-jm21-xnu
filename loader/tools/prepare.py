from pathlib import Path
import hashlib,struct,os,subprocess
import adt,mkpayload
# The kernel built from source (this repository: ARMv8.0 flags, LSE emulation,
# vtimer_intid, kern.jm21_bridge, the eMMC driver). JM21_KERNEL= selects another; the pinned prebuilt
# (PINNED) still works through the binary patches below, which the source kernel does not need.
kernel=Path(os.environ.get('JM21_KERNEL','../hakios-kernel'))
b=bytearray(kernel.read_bytes())
PINNED=hashlib.sha256(b).hexdigest()=='03e794e8e1d389a6abb9bcd2431eaccf9747271ff8c1234922225718c70441e9'
lowram=os.environ.get('JM21_LOW_RAM')=='1'
assert not (PINNED and lowram), 'JM21_LOW_RAM requires the source kernel'
print('kernel:',kernel,'(pinned prebuilt)' if PINNED else '(source build)')
# The monitor reads the kernel log through its msgbuf pointer symbol.
syms=subprocess.run(['llvm-nm',str(kernel)],check=True,capture_output=True,text=True).stdout
msgbufp=next(int(line.split()[0],16) for line in syms.splitlines() if line.endswith(' _msgbufp'))
symbol_map={line.split()[-1]:int(line.split()[0],16) for line in syms.splitlines() if len(line.split())==3}
if PINNED: subprocess.run(['aarch64-linux-gnu-ld','-T','src/shell-cache.ld','--entry=shell_read_copy','--build-id=none',
 '--defsym=strlcpy_chk=0xfffffff007406858']+
 [f'--defsym={name}={symbol_map["_"+name]:#x}' for name in ['uio_offset','uio_resid','uiomove']]+
 ['out/shell-cache.o','out/shell-vfs.o','-o','out/shell-cache.elf'],check=True)
if PINNED: subprocess.run(['aarch64-linux-gnu-objcopy','-O','binary','--only-section=.text','--only-section=.rodata',
 'out/shell-cache.elf','out/shell-cache.bin'],check=True)
# Dedicated 256 bytes in 64 KiB of padding after the APFS container, inside the ramdisk
# segment the kernel maps (memory above kernel_end is not in its static mapping).
apfs=Path('out/root.apfs').read_bytes()
# APFS uses 4 KiB blocks; the XNU ramdisk segment needs 16 KiB alignment.
apfs += bytes((-len(apfs)) % 0x4000)
Path('out/ramdisk.img').write_bytes(apfs+bytes(0x10000))
segs=mkpayload.parse_macho(b)['segs']
ramdisk_va=(max(x['vmaddr']+x['vmsize'] for x in segs)+mkpayload.PAGE-1)&~(mkpayload.PAGE-1)  # as mkpayload.build
hostname=ramdisk_va+len(apfs)
Path('out/usb-kernel.h').write_text(
 f'#define XNU_JM21_LOW_RAM {int(lowram)}\n'
 f'#define XNU_VIRT_BASE {0xfffffff004000000-(0x40000000 if lowram else 0):#x}UL\n'
 f'#define XNU_PHYS_BASE {0x40000000 if lowram else 0x80000000:#x}UL\n'
 f'#define XNU_RAM_SIZE {0xc0000000 if lowram else 0x80000000:#x}UL\n'
 f'#define XNU_HOSTNAME_PA {hostname-0xfffffff004000000+0x80000000:#x}UL\n'
 f'#define XNU_MSGBUFP_PA {msgbufp-0xfffffff004000000+0x80000000:#x}UL\n')
m=mkpayload.parse_macho(b)
def patch(va,old,new):
 s=next(s for s in m['segs'] if s['vmaddr']<=va<s['vmaddr']+s['filesize'])
 o=s['fileoff']+va-s['vmaddr']; assert struct.unpack_from('<I',b,o)[0]==old,hex(va)
 struct.pack_into('<I',b,o,new)
if PINNED:
 # Route only kern.hostname to an isolated mailbox and synchronize the Gold cache.
 # The original hostname_lock shares its cache line with _hostname: do not invalidate it.
 def adrp(pc, target, reg):
  delta=(target>>12)-(pc>>12)
  return 0x90000000|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)|reg
 patch(0xfffffff0077cd9f0,0x90002d41,adrp(0xfffffff0077cd9f0,hostname,1))
 patch(0xfffffff0077cd9f4,0x9107c021,0x91000021)
 patch(0xfffffff0077cda48,0x90002d40,adrp(0xfffffff0077cda48,hostname,0))
 patch(0xfffffff0077cda4c,0x9107c000,0x91000000)
 cave=0xfffffff007b27890
 stub=Path('out/shell-cache.bin').read_bytes()
 assert len(stub)<=1904 and len(stub)%4==0
 for off in range(0,len(stub),4):patch(cave+off,0,struct.unpack_from('<I',stub,off)[0])
 cache_syms=subprocess.run(['aarch64-linux-gnu-nm','out/shell-cache.elf'],check=True,capture_output=True,text=True).stdout
 write_copy=next(int(line.split()[0],16) for line in cache_syms.splitlines() if line.endswith(' shell_write_copy'))
 for pc,old,target in [(0xfffffff0077cda04,0x97f0e395,cave),(0xfffffff0077cda5c,0x97f0e37f,write_copy)]:
  patch(pc,old,0x94000000|(((target-pc)//4)&0x3ffffff))
 # Enable only mockfs open/close/access and enumeration; retain write errors.
 cache_map={line.split()[-1]:int(line.split()[0],16) for line in cache_syms.splitlines() if len(line.split())==3}
 for va,old,name in [(0xfffffff0071d8c38,symbol_map['_err_open'],'shell_vfs_nop'),
  (0xfffffff0071d8c58,symbol_map['_err_close'],'shell_vfs_nop'),
  (0xfffffff0071d8c68,symbol_map['_err_access'],'shell_vfs_nop'),
  (0xfffffff0071d8d58,symbol_map['_err_readdir'],'shell_vfs_readdir')]:
  target=cache_map[name]
  patch(va,old&0xffffffff,target&0xffffffff)
  patch(va+4,old>>32,target>>32)
 # JM21 Qualcomm timer: DT GIC_PPI 3 is interrupt 19, not generic 27.
 patch(0xfffffff007457fb8,0x72a10008,0x72a00108)
 patch(0xfffffff007457fc0,0x52a10008,0x52a00108)
 patch(0xfffffff00745805c,0x52800369,0x52800269)
 patch(0xfffffff00745813c,0x71006e9f,0x71004e9f)
 patch(0xfffffff007457fe4,0xd123952a,0xd123b52a)
 # Diagnostic-only kernel: older Kryo cores lack PAN. Remove PAN accesses and report the kernel PAN state as enabled;
 # no user space or disk driver is started by this debug image.
 counts=0
 for seg in m['segs']:
  if seg['name'] != '__TEXT_EXEC': continue
  for off in range(seg['fileoff'],seg['fileoff']+seg['filesize'],4):
   op=struct.unpack_from('<I',b,off)[0]
   if op in (0xd500409f,0xd500419f): new=0xd503201f
   elif op & 0xffffffe0 == 0xd5384260: new=0xd2a00800|(op&31) # mov xN, #0x400000 (PSTATE.PAN)
   elif op & 0xffffffe0 == 0xd5184260: new=0xd503201f
   else:continue
   struct.pack_into('<I',b,off,new);counts+=1
 print('PAN compatibility substitutions:',counts)
prefix=b'[%5lu.%06u]: '
if b.count(prefix+b'\0')==1:
 pos=b.index(prefix+b'\0');b[pos:pos+len(prefix)]=bytes(len(prefix))
Path('out/kernel-jm21.macho').write_bytes(b)
def node(name,**props):
 n=adt.Node(); props={'name':name,**props}
 for k,v in props.items():
  if isinstance(v,str):v=v.encode()+b'\0'
  n.props.append((k,0,v))
 return n
u32=lambda n:struct.pack('<I',n)
u64=lambda n:struct.pack('<Q',n)
r=node('device-tree',model='FiiO JM21',compatible='arm-io',**{'#address-cells':u32(2),'#size-cells':u32(2),'platform-name':'bcm2837'})
# Fixed seed keeps images reproducible; JM21_RANDOM_SEED=1 restores per-build entropy.
seed=os.urandom(64) if os.environ.get('JM21_RANDOM_SEED')=='1' else hashlib.sha512(b'jm21-xnu-poc-display').digest()
c=node('chosen',**{'dram-base':u64(0x80000000),'dram-size':u64(0x80000000),'embedded-panic-log-size':u32(0x80000),'debug-enabled':u32(1),'psci-method':'smc','random-seed':seed})
import zlib
nv=bytearray(0x2000)
for off,sig,size,name in [(0,0x5a,0x20,b'nvram'),(0x20,0x70,0x1fe0,b'common')]:
 nv[off]=sig;struct.pack_into('<H',nv,off+2,size//16);nv[off+4:off+4+len(name)]=name
 total=sum(nv[off:off+16]);nv[off+1]=(total&255)+(total>>8)
struct.pack_into('<I',nv,20,1);struct.pack_into('<I',nv,16,zlib.adler32(nv[20:]))
c.props.extend([('nvram-bank-size',0,u32(0x2000)),('nvram-bank-count',0,u32(1)),('nvram-current-bank',0,u32(0)),('nvram-proxy-data',0,bytes(nv))])
c.children=[node('memory-map')]
# XNU boots on Gold 0x100; the other Golds follow, then Silver 1-3 as small cores.
# Silver 0 runs the USB monitor and is never given to the kernel. cpus=N takes the first N.
def cpu(i,mpidr):
 extra={'small-core':u32(1)} if mpidr<0x100 else {}
 return node(f'cpu{i}',device_type='cpu',state='running' if i==0 else 'waiting',reg=u32(mpidr),**{'cpu-id':u32(i),'timebase-frequency':u64(19200000),'fixed-frequency':u64(19200000),'clock-frequency':u64(1800000000),'bus-frequency':u64(100000000),'memory-frequency':u64(100000000),'peripheral-frequency':u64(19200000)},**extra)
cp=node('cpus'); cp.children=[cpu(i,m) for i,m in enumerate([0x100,0x101,0x102,0x103,0x1,0x2,0x3])]
io=node('arm-io',device_type='soc',ranges=struct.pack('<3Q',0,0x1000000,0x20000000),**{'#address-cells':u32(2),'#size-cells':u32(2)})
io.children=[node('gic',device_type='interrupt-controller',compatible='arm,gic-v3',reg=struct.pack('<4Q',0xe200000,0x10000,0xe300000,0x100000))]
r.children=[c,cp,io,node('options'),node('pram',reg=struct.pack('<2Q',0x8a000000,0x80000)),node('defaults',**{'kern.vm_compressor':u32(2)})]
if os.environ.get('JM21_HOME')=='1':
 # iOS identity from the matching, locally extracted IPSW. Hardware nodes stay JM21.
 identity,_=adt.parse(Path('out/iphone-d22.adt').read_bytes())
 skip={'name','#address-cells','#size-cells','AAPL,phandle','clock-frequency',
       'platform-name','time-stamp','secure-root-prefix'}
 kept=[p for p in identity.props if p[0] not in skip and not p[2].startswith((b'syscfg/',b'macaddr/',b'zeroes/'))]
 names={p[0] for p in kept}
 r.props=[p for p in r.props if p[0] not in names]+kept
 product=next(n for n in identity.children if n.name=='product')
 product.props=[p for p in product.props if p[0]!='AAPL,phandle' and not p[2].startswith((b'syscfg/',b'macaddr/',b'zeroes/'))]
 product.children=[]
 # Native 16:9 panel: 375x667 points at 2x (the iPhone 8 screen class) instead of the X's 2436@3x.
 screen={'artwork-scale-factor':u32(2),'artwork-device-subtype':u32(1334)}
 product.props=[(k,f,screen.get(k,v)) for k,f,v in product.props]
 # The panel/framebuffer is 8-bit sRGB. The iPhone X's "P3" makes MobileGestalt report an extended-colour display and
 # WebKit then draws page tiles into 10-bit surfaces ('w30r'/'b3a8') that the kernel's IOSurfaces and the window
 # server cannot make or show (redwood's loader dt_identity() does the same). JM21_GAMUT=p3 keeps the iPhone's value.
 if os.environ.get('JM21_GAMUT','srgb')!='p3':
  product.props=[(k,f,b'sRGB\0' if k=='artwork-display-gamut' else v) for k,f,v in product.props]
 r.children.append(product)
 r.props.append(('serial-number',0,b'JM21XNU00001'.ljust(32,b'\0')))
 c.props.extend(node('chosen',**{'display-scale':u32(2),'display-rotation':u32(0),
  'unique-chip-id':u64(0x4a4d32310001),'chip-id':u32(0x8015),'board-id':u32(6),
  'mac-address-wifi0':bytes.fromhex('024a4d323101'),
  'mac-address-bluetooth0':bytes.fromhex('024a4d323102')}).props[1:])
Path('out/jm21.adt').write_bytes(adt.serialize(r))
args='-v debug=0x104 keepsyms=1 cpus=7 small_cores_after=0 bootcpu=0x100 serial=0 wdt=-1 panic_reboot=0 rd=md0'
if os.environ.get('JM21_SILENT')=='1':
 args=args.replace('-v ', '').replace('debug=0x104', 'debug=0x100')
if not PINNED:
 args+=f' vtimer_intid=19 jm21_bridge={hostname-0xfffffff004000000+0x80000000:#x} mmc=1 jm21_ios=1'
if os.environ.get('JM21_HOME')=='1': args+=' fbwidth=750 fbheight=1334'
if os.environ.get('JM21_TOUCH')=='1': args+=' jm21_touch=1'
touch_poll=os.environ.get('JM21_TOUCH_POLL_MS','')
if touch_poll:
 touch_poll=int(touch_poll)
 if not 1 <= touch_poll <= 20: raise ValueError('JM21_TOUCH_POLL_MS must be 1..20')
 args+=f' jm21_touch_poll_ms={touch_poll}'
touch_poll_us=os.environ.get('JM21_TOUCH_POLL_US','')
if touch_poll_us:
 touch_poll_us=int(touch_poll_us)
 if not 100 <= touch_poll_us <= 20000: raise ValueError('JM21_TOUCH_POLL_US must be 100..20000')
 args+=f' jm21_touch_poll_us={touch_poll_us}'
if os.environ.get('JM21_BUTTONS')=='1': args+=' jm21_buttons=1'
if os.environ.get('JM21_BATTERY')=='1': args+=' jm21_battery=1'+('' if os.environ.get('JM21_CHARGE')=='0' else ' jm21_charge=1')
if os.environ.get('JM21_A610')=='1': args+=' jm21_a610=1'
if os.environ.get('JM21_WIFI','0') not in ('','0'): args+=' jm21_wifi='+os.environ['JM21_WIFI']
if os.environ.get('JM21_GPU')=='1': args+=' jm21_gpu=1'
if os.environ.get('JM21_MDP')=='1': args+=' jm21_mdp=1'
# JM21_VSYNC=1: publish frames on the panel's TE pulse (TLMM GPIO81, read only); 2 = measure only.
if os.environ.get('JM21_VSYNC') in ('1','2'): args+=' jm21_vsync='+os.environ['JM21_VSYNC']
if os.environ.get('JM21_MMC_HS400')=='1': args+=' mmc_hs400=1'
# JM21_FASTIO=1: the eMMC (hakios mmc) is not a memory disk; let APFS cluster metadata/compression I/O.
if os.environ.get('JM21_FASTIO')=='1': args+=' apfs_cheap=0'
if os.environ.get('JM21_MIGRATED')=='1': args+=' jm21_migrated=1'
# Experimental Clutch timeslice cap; omitted keeps Apple's scheduler defaults.
quantum_us=os.environ.get('JM21_QUANTUM_US','')
if quantum_us:
 quantum_us=int(quantum_us)
 if not 500 <= quantum_us <= 10000: raise ValueError('JM21_QUANTUM_US must be 500..10000')
 args+=f' jm21_quantum_us={quantum_us} preempt={1000000//quantum_us} bg_preempt={1000000//quantum_us}'
# JM21_EXTRA_ARGS: free-form extra boot-args (e.g. mmc_bench=64 jm21_als_ms=300).
if os.environ.get('JM21_EXTRA_ARGS'): args+=' '+os.environ['JM21_EXTRA_ARGS'].strip()
if lowram: args+=' jm21_lowram=1'
if os.environ.get('JM21_PERSIST')=='1': args=args.replace(' mmc=1',' mmc=2')+' var_store=1'
if os.environ.get('JM21_DATA_HFS')=='1':
 if os.environ.get('JM21_HOME')!='1': raise ValueError('HFS Data requires the Home profile')
 window=os.environ.get('JM21_DATA_WINDOW','')
 import re
 if window!='super' and not re.fullmatch(r'[0-9]{1,7}:[0-9]{1,7}',window):
  raise ValueError('JM21_DATA_WINDOW must be START_MIB:SIZE_MIB from a verified, provisioned layout')
 if window!='super':
  first,size=map(int,window.split(':'))
  if first<12288 or size<64: raise ValueError('Data must exclude [0,12 GiB) and be at least 64 MiB')
 args=args.replace(' mmc=1',' mmc=2').replace(' var_store=1','')
 args+=f' mmc_data={window}'
 if os.environ.get('JM21_DATA_MIGRATE')=='1': args+=' dm=1'
# Save command-line space without changing runtime features (symbolic panic names only).
if os.environ.get('JM21_COMPACT_BOOTARGS')=='1': args=args.replace(' keepsyms=1','')
blob,info=mkpayload.build('out/kernel-jm21.macho','out/jm21.adt',args,'out/ramdisk.img')
blob=bytearray(blob)
assert info['ramdisk'][0]==hex(ramdisk_va-0xfffffff004000000+0x80000000),info['ramdisk']
assert int(info['kernel_end'],16)-0xfffffff004000000+0x80000000+0x8000+len(Path('out/jm21.adt').read_bytes())<0x8a000000-0x4000, ('payload overlaps the panic region at 0x8a000000', info['kernel_end'])
# Match the generic kernel's original loader ABI (48 MB text offset).
struct.pack_into('<Q',blob,0x10,0xfffffff004000000)
info['virt_base']='0xfffffff004000000'
Path('out/payload.bin').write_bytes(blob)
Path('out/payload_size.h').write_text(f'#define PAYLOAD_SIZE {len(blob)}UL\n')
print({k:v for k,v in info.items() if k!='segs'})
