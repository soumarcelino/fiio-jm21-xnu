#!/usr/bin/env python3
"""USB client for the JM21 XNU userspace shell and loader diagnostics.

PID 1 bridges /bin/sh (static, APFS root on md0) through a pty; this is not iOS userland.
Only the experimental USB identity plus the expected serial is accepted.
"""
import argparse
import ctypes as C
import ctypes.util
from pathlib import Path
import shlex
import sys
import time

VID, PID = 0x1209, 0x4A21
SERIAL = "JM21-XNU-1"
CHUNK = 1024


class Descriptor(C.Structure):
    _fields_ = [("length", C.c_uint8), ("type", C.c_uint8),
                ("usb", C.c_uint16), ("cls", C.c_uint8),
                ("subcls", C.c_uint8), ("protocol", C.c_uint8),
                ("max_packet", C.c_uint8), ("vid", C.c_uint16),
                ("pid", C.c_uint16), ("version", C.c_uint16),
                ("manufacturer", C.c_uint8), ("product", C.c_uint8),
                ("serial", C.c_uint8), ("configs", C.c_uint8)]


class Monitor:
    def __init__(self):
        name = ctypes.util.find_library("usb-1.0")
        if not name:
            raise RuntimeError("libusb-1.0 não encontrada")
        self.lib = C.CDLL(name)
        signatures = {
            "init": ([C.POINTER(C.c_void_p)], C.c_int),
            "exit": ([C.c_void_p], None),
            "get_device_list": ([C.c_void_p, C.POINTER(C.POINTER(C.c_void_p))], C.c_ssize_t),
            "free_device_list": ([C.POINTER(C.c_void_p), C.c_int], None),
            "get_device_descriptor": ([C.c_void_p, C.POINTER(Descriptor)], C.c_int),
            "open": ([C.c_void_p, C.POINTER(C.c_void_p)], C.c_int),
            "close": ([C.c_void_p], None),
            "get_string_descriptor_ascii": ([C.c_void_p, C.c_uint8, C.c_void_p, C.c_int], C.c_int),
            "get_configuration": ([C.c_void_p, C.POINTER(C.c_int)], C.c_int),
            "set_configuration": ([C.c_void_p, C.c_int], C.c_int),
            "control_transfer": ([C.c_void_p, C.c_uint8, C.c_uint8, C.c_uint16,
                                   C.c_uint16, C.c_void_p, C.c_uint16, C.c_uint], C.c_int),
            "error_name": ([C.c_int], C.c_char_p),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.lib, "libusb_" + name)
            fn.argtypes, fn.restype = args, result
        self.context, self.handle = C.c_void_p(), C.c_void_p()
        self.check(self.lib.libusb_init(C.byref(self.context)))
        devices = C.POINTER(C.c_void_p)()
        try:
            count = self.check(self.lib.libusb_get_device_list(self.context, C.byref(devices)))
            try:
                matches = []
                for i in range(count):
                    desc = Descriptor()
                    self.check(self.lib.libusb_get_device_descriptor(devices[i], C.byref(desc)))
                    if (desc.vid, desc.pid) == (VID, PID):
                        matches.append((devices[i], desc.serial))
                if len(matches) != 1:
                    raise RuntimeError(f"Esperado um JM21 USB monitor; encontrados {len(matches)}")
                device, serial_index = matches[0]
                self.check(self.lib.libusb_open(device, C.byref(self.handle)))
                serial = C.create_string_buffer(256)
                n = self.check(self.lib.libusb_get_string_descriptor_ascii(self.handle, serial_index, serial, 256))
                if serial.raw[:n].decode("ascii", errors="replace") != SERIAL:
                    raise RuntimeError("Serial USB inesperado; dispositivo recusado")
                config = C.c_int()
                self.check(self.lib.libusb_get_configuration(self.handle, C.byref(config)))
                if config.value != 1:
                    self.check(self.lib.libusb_set_configuration(self.handle, 1))
            finally:
                if devices:
                    self.lib.libusb_free_device_list(devices, 1)
        except BaseException:
            self.close()
            raise

    def check(self, result):
        if result < 0:
            raise RuntimeError(self.lib.libusb_error_name(result).decode())
        return result

    def read(self, command, index=0, size=CHUNK):
        buf = C.create_string_buffer(size)
        n = self.check(self.lib.libusb_control_transfer(
            self.handle, 0xC0, 0x40, command, index, buf, size, 10000))
        return buf.raw[:n]

    def dump(self, command, size):
        result = bytearray()
        for offset in range(0, size, CHUNK):
            want = min(CHUNK, size - offset)
            block = self.read(command, offset // CHUNK, want)
            if len(block) != want:
                raise RuntimeError(f"Resposta truncada no offset {offset}")
            result.extend(block)
        return bytes(result)

    def close(self):
        if self.handle:
            self.lib.libusb_close(self.handle)
            self.handle = C.c_void_p()
        if self.context:
            self.lib.libusb_exit(self.context)
            self.context = C.c_void_p()


def shell_frame(monitor):
    frame = monitor.read(11, size=256)
    try:
        body, checksum = frame.rsplit(b":", 1)
        value = 2166136261
        for byte in body:
            value = ((value ^ byte) * 16777619) & 0xffffffff
        if value != int(checksum, 16) or body[5:6] != b":":
            return None
        return chr(body[0]), int(body[1:5], 16), body[6:]
    except (ValueError, IndexError):
        return None


PROMPT = b"jm21# "


class ShellStream:
    """Byte stream to /bin/sh on the PID 1 pty: input frames C, output frames O (acknowledged)."""

    def __init__(self, monitor):
        self.monitor = monitor
        self.last = None

    def poll(self):
        """Return (output bytes, ready) for one mailbox read."""
        frame = shell_frame(self.monitor)
        if not frame:
            return b"", False
        kind, seq, payload = frame
        if kind == "O":
            data = b""
            if seq != self.last:
                data = bytes.fromhex(payload.decode("ascii"))
                self.last = seq
            self.monitor.read(12, seq)
            return data, False
        return b"", kind == "R"

    def send(self, data, sink):
        for start in range(0, len(data), 220):
            chunk = data[start:start + 220]
            if b"\0" in chunk:
                raise ValueError("Entrada com NUL")
            # PID 1 may publish output between our read of R and the commit: retry.
            for _ in range(100):
                if self._send_chunk(chunk, sink):
                    break
            else:
                raise RuntimeError("Shell XNU ocupado")

    def _send_chunk(self, chunk, sink):
        deadline = time.monotonic() + 60
        while True:
            frame = shell_frame(self.monitor)
            if frame and frame[0] == "R":
                break
            if frame and frame[0] == "O":
                out, _ = self.poll()
                sink(out)
                deadline = time.monotonic() + 60
            elif time.monotonic() > deadline:
                raise RuntimeError(f"Shell XNU indisponível: {frame}")
            time.sleep(0.01)
        requests = [(9, 65535)] + [(9, (i << 8) | b) for i, b in enumerate(chunk)] + [(10, (frame[1] + 1) & 0xffff)]
        for operation, index in requests:
            reply = self.monitor.read(operation, index)
            if reply == b"ERR busy":
                return False
            if reply != b"OK":
                raise RuntimeError(reply.decode("utf-8", errors="replace"))
        return True


def write_out(data):
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()


def shell_command(monitor, line):
    """Run one line in /bin/sh and print its output, without the prompt that follows it."""
    stream = ShellStream(monitor)
    # Output pending from before this command (an earlier prompt) is not its output.
    stream.send(line.encode("utf-8") + b"\n", lambda data: None)
    buffer = bytearray()
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        data, ready = stream.poll()
        if data:
            buffer += data
            deadline = time.monotonic() + 60
            out = bytes(buffer)
            keep = len(PROMPT)
            if len(out) > keep:
                write_out(out[:-keep].replace(b"\r\n", b"\n"))
                del buffer[:-keep]
        if ready and bytes(buffer).endswith(PROMPT):
            write_out(bytes(buffer[:-len(PROMPT)]).replace(b"\r\n", b"\n"))
            return
        time.sleep(0.005)
    raise RuntimeError("Shell XNU sem resposta por 60 segundos")


def shell_interactive(monitor, output_dir):
    import select
    print("/bin/sh no XNU via USB. Ctrl-C interrompe, Ctrl-D sai; ~log, ~cpu, ~reboot bootloader vão ao monitor.")
    stream = ShellStream(monitor)
    stream.send(b"\n", write_out)
    while True:
        try:
            data, _ = stream.poll()
            write_out(data.replace(b"\r\n", b"\n"))
            if select.select([sys.stdin], [], [], 0 if data else 0.02)[0]:
                line = sys.stdin.readline()
                if not line:
                    print()
                    return True
                if line.startswith("~"):
                    if not execute(monitor, shlex.split(line[1:]), output_dir):
                        return False
                    stream.send(b"\n", write_out)
                    continue
                stream.send(line.encode("utf-8"), write_out)
        except KeyboardInterrupt:
            stream.send(b"\x03", write_out)


def execute(monitor, words, output_dir):
    if not words:
        return True
    cmd, *args = words
    if cmd in ("sh", "shell"):
        if args:
            shell_command(monitor, args[0] if len(args) == 1 else shlex.join(args))
            return True
        return shell_interactive(monitor, output_dir)
    if cmd in ("exit", "quit"):
        return False
    if cmd in ("reboot", "bootloader", "recovery", "poweroff", "shutdown"):
        if cmd == "shutdown":
            if args == ["-h", "now"]:
                cmd, args = "poweroff", []
            elif args == ["-r", "now"]:
                cmd, args = "reboot", []
            else:
                raise ValueError("Uso: shutdown -h now | shutdown -r now")
        if cmd in ("bootloader", "recovery"):
            if args:
                raise ValueError(f"{cmd} não aceita argumentos")
            args, cmd = [cmd], "reboot"
        if cmd == "poweroff":
            if args:
                raise ValueError("poweroff não aceita argumentos")
            target = 3
        elif args in ([], ["normal"]):
            target = 0
        elif args == ["bootloader"]:
            target = 1
        elif args == ["recovery"]:
            target = 2
        else:
            raise ValueError("Uso: reboot [normal|bootloader|recovery]")
        protocol = monitor.read(0).decode("utf-8", errors="replace")
        if target and not protocol.startswith("JM21 monitor protocol 2\n"):
            raise RuntimeError("Firmware antigo: destino de reboot/poweroff indisponível")
        print(monitor.read(5, target).decode("utf-8", errors="replace"), end="", flush=True)
        # The USB device is about to disappear; leave the interactive session.
        return False
    if cmd in ("help", "info", "uptime", "diag", "cpu", "resetreason", "intfscan"):
        if args:
            raise ValueError(f"{cmd} não aceita argumentos")
        command = {"help": 0, "info": 1, "uptime": 2, "diag": 8, "cpu": 13, "resetreason": 16, "intfscan": 19}[cmd]
        print(monitor.read(command).decode("utf-8", errors="replace"), end="", flush=True)
    elif cmd == "lastpanic":
        # The previous boot's panic region, kept by the loader at 0x8a080000 (pages 512+ of command 3).
        data = b"".join(monitor.read(3, 512 + i) for i in range(512))
        text = data.split(b"\0", 1)[0] if data[:1] != b"\0" else data.lstrip(b"\0").split(b"\0", 1)[0]
        print(text.decode("utf-8", errors="replace") or "(vazio)")
    elif cmd == "emmc":
        # Read-only probe of the eMMC through the loader: controller state, then the GPT.
        print(monitor.read(15).decode("utf-8", errors="replace"), end="")
        sectors = {}
        def sector(lba):
            data = monitor.read(14, lba, 512)
            if len(data) != 512:
                raise RuntimeError(data.decode("utf-8", errors="replace"))
            sectors[lba] = data
            return data
        header = sector(1)
        if header[:8] != b"EFI PART":
            raise RuntimeError(f"LBA 1 sem GPT: {header[:16].hex()}")
        import struct
        first, count, size = struct.unpack_from("<QII", header, 72)
        disk_last = struct.unpack_from("<Q", header, 48)[0]
        print(f"GPT ok: {count} entradas de {size} bytes a partir do LBA {first}; último LBA utilizável {disk_last}")
        rows = []
        for lba in range(first, first + (count * size + 511) // 512):
            block = sector(lba)
            for off in range(0, 512, size):
                entry = block[off:off + size]
                if entry[:16] == bytes(16):
                    continue
                start, end = struct.unpack_from("<QQ", entry, 32)
                name = entry[56:128].decode("utf-16-le", errors="replace").rstrip("\0")
                rows.append((start, end, name))
        for start, end, name in rows:
            print(f"{name:24} {start:>10} {end:>10} {(end - start + 1) * 512 / 2**20:10.1f} MiB")
        path = output_dir / "emmc-gpt.bin"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"".join(sectors[k] for k in sorted(sectors)))
        print(path)
    elif cmd == "logwatch":
        # tail -f of the kernel log: needs the kernel's periodic msgbuf clean (hakios.jm21.c)
        last = b""
        while True:
            response = monitor.read(7).decode("utf-8", errors="replace").strip()
            prefix = "Log snapshot bytes: "
            if response.startswith(prefix):
                data = monitor.dump(6, int(response[len(prefix):])).replace(b"\0", b"")
                if last:
                    key = last[-160:]
                    at = data.rfind(key)
                    new = data[at + len(key):] if at >= 0 else data
                else:
                    new = data[-3000:]
                if new:
                    text = new.decode("utf-8", errors="replace")
                    print("".join(c if c in "\n\t" or c.isprintable() else "?" for c in text), end="", flush=True)
                last = data
            time.sleep(1)
    elif cmd == "log":
        if len(args) > 1:
            raise ValueError("Uso: log [arquivo no PC]")
        response = monitor.read(7).decode("utf-8", errors="replace").strip()
        prefix = "Log snapshot bytes: "
        if not response.startswith(prefix):
            raise RuntimeError(response)
        size = int(response[len(prefix):])
        if not 0 < size <= 1024 * 1024:
            raise RuntimeError("Tamanho inválido de log")
        log = monitor.dump(6, size).replace(b"\0", b"")
        if args:
            with Path(args[0]).open("xb") as stream:
                stream.write(log)
            print(args[0])
            return True
        # Render control bytes as escapes to avoid terminal escape injection.
        content = log.decode("utf-8", errors="replace")
        print("".join(c if c in "\n\t" or c.isprintable() else repr(c)[1:-1]
                      for c in content))
    elif cmd in ("screen", "panic"):
        if len(args) > 1:
            raise ValueError(f"Uso: {cmd} [arquivo]")
        output_dir.mkdir(parents=True, exist_ok=True)
        path = Path(args[0]) if args else output_dir / ("screen.ppm" if cmd == "screen" else "panic.bin")
        if path.exists():
            raise ValueError(f"Arquivo já existe: {path}")
        if cmd == "panic":
            blob = monitor.dump(3, 0x80000)
        else:
            raw = monitor.dump(4, 750 * 1334 * 4)
            # Firmware framebuffer is BGRX8888, rows have no padding.
            rgb = bytearray(750 * 1334 * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
            blob = b"P6\n750 1334\n255\n" + rgb
        with path.open("xb") as stream:
            stream.write(blob)
        print(path)
    else:
        raise ValueError("Comando desconhecido. Use help, info, uptime, log, screen, panic, reboot [bootloader|recovery], poweroff ou exit")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent.parent / "evidence" / "usb")
    parser.add_argument("command", nargs="*", help="sem comando: console interativo")
    options = parser.parse_args()
    monitor = None
    try:
        monitor = Monitor()
        if options.command:
            execute(monitor, options.command, options.output_dir)
        elif b"userspace shell" in monitor.read(0):
            execute(monitor, ["sh"], options.output_dir)
        else:
            print("JM21 console administrativo do loader (EL1). help para comandos; exit para sair.")
            while True:
                try:
                    if not execute(monitor, shlex.split(input("jm21-monitor# ")), options.output_dir):
                        break
                except ValueError as error:
                    print(error, file=sys.stderr)
    except (RuntimeError, OSError, ValueError) as error:
        print(f"USB: {error}", file=sys.stderr)
        return 1
    except (EOFError, KeyboardInterrupt):
        print()
    finally:
        if monitor:
            monitor.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
