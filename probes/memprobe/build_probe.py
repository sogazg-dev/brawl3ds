"""Build the brawl3ds memory/CPU probe as two CIAs: one asking for 124 MB, one for 178 MB.

Usage: python -I probes/memprobe/build_probe.py [--makerom PATH] [--devkitpro C:/devkitPro]
Output: probes/memprobe/build/brawl3ds-probe-124mb.cia and brawl3ds-probe-178mb.cia

The two CIAs differ only in the exheader's SystemModeExt (and their title IDs), so installing both and running
each shows what this console really grants in each mode.
"""
import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
VARIANTS = {  # name: (SystemModeExt, UniqueId). 0xBB3D_ is an arbitrary homebrew-range id for this project.
    "124mb": ("124MB", "0xBB3D0"),
    "178mb": ("178MB", "0xBB3D1"),
}

RSF = """BasicInfo:
  Title: B3DSPROBE
  ProductCode: CTR-P-B3DP
  Logo: Homebrew
TitleInfo:
  Category: Application
  UniqueId: {unique_id}
Option:
  UseOnSD: true
  FreeProductCode: true
  MediaFootPadding: false
  EnableCrypt: false
  EnableCompress: true
AccessControlInfo:
  CoreVersion: 2
  DescVersion: 2
  ReleaseKernelMajor: "02"
  ReleaseKernelMinor: "50"
  UseExtSaveData: false
  FileSystemAccess:
   - DirectSdmc
   - DirectSdmcWrite
  MemoryType: Application
  SystemMode: 64MB
  SystemModeExt: {system_mode_ext}
  CpuSpeed: 804MHz
  EnableL2Cache: true
  CanAccessCore2: true
  IdealProcessor: 0
  AffinityMask: 1
  Priority: 16
  MaxCpu: 0x9E
  HandleTableSize: 0x200
  DisableDebug: false
  EnableForceDebug: false
  CanWriteSharedPage: true
  CanUsePrivilegedPriority: false
  CanUseNonAlphabetAndNumber: true
  PermitMainFunctionArgument: true
  CanShareDeviceMemory: true
  RunnableOnSleep: false
  SpecialMemoryArrange: true
  SystemCallAccess:
    ArbitrateAddress: 34
    Break: 60
    CancelTimer: 28
    ClearEvent: 25
    ClearTimer: 29
    CloseHandle: 35
    ConnectToPort: 45
    ControlMemory: 1
    CreateAddressArbiter: 33
    CreateEvent: 23
    CreateMemoryBlock: 30
    CreateMutex: 19
    CreateSemaphore: 21
    CreateThread: 8
    CreateTimer: 26
    DuplicateHandle: 39
    ExitProcess: 3
    ExitThread: 9
    FlushProcessDataCache: 84
    GetCurrentProcessorNumber: 17
    GetHandleInfo: 41
    GetProcessId: 53
    GetProcessIdOfThread: 54
    GetProcessInfo: 43
    GetResourceLimit: 56
    GetResourceLimitCurrentValues: 58
    GetResourceLimitLimitValues: 57
    GetSystemInfo: 42
    GetSystemTick: 40
    GetThreadId: 55
    GetThreadPriority: 11
    MapMemoryBlock: 31
    OutputDebugString: 61
    QueryMemory: 2
    ReleaseMutex: 20
    ReleaseSemaphore: 22
    SendSyncRequest: 50
    SetThreadPriority: 12
    SetTimer: 27
    SignalEvent: 24
    SleepThread: 10
    UnmapMemoryBlock: 32
    WaitSynchronization1: 36
    WaitSynchronizationN: 37
  ServiceAccessControl:
   - APT:U
   - cfg:u
   - fs:USER
   - gsp::Gpu
   - hid:USER
   - ptm:u
SystemControlInfo:
  SaveDataSize: 0KB
  RemasterVersion: 0
  StackSize: 0x40000
  Dependency:
    cfg: 0x0004013000001702
    gsp: 0x0004013000001c02
    hid: 0x0004013000001d02
    ptm: 0x0004013000002202
"""


def png(width, height, pixel):
    """A tiny RGBA PNG writer (no third-party imaging library needed)."""
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def icon_pixel(x, y):
    # A plain gauge-like icon: dark background, blue bar rising from the bottom.
    inside = 6 <= x < 42 and 6 <= y < 42
    bar = 14 <= x < 34 and y >= 18 and inside
    return (40, 140, 230, 255) if bar else (30, 30, 38, 255) if inside else (0, 0, 0, 0)


def run(cmd):
    print(">", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--devkitpro", default="C:/devkitPro")
    ap.add_argument("--makerom", default=str(Path.home() / "brawl3ds-work/tools/makerom/makerom.exe"))
    args = ap.parse_args()
    dkp = Path(args.devkitpro)
    gcc = dkp / "devkitARM/bin/arm-none-eabi-gcc.exe"
    smdhtool = dkp / "tools/bin/smdhtool.exe"
    for tool in (gcc, smdhtool, Path(args.makerom)):
        if not tool.is_file():
            sys.exit(f"missing tool: {tool}")
    BUILD.mkdir(exist_ok=True)

    icon_png = BUILD / "icon.png"
    icon_png.write_bytes(png(48, 48, icon_pixel))
    smdh = BUILD / "icon.smdh"
    run([smdhtool, "--create", "brawl3ds probe", "Memory and CPU probe", "brawl3ds", icon_png, smdh])

    arch = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]
    for name, (mode, unique_id) in VARIANTS.items():
        elf = BUILD / f"probe-{name}.elf"
        run([gcc, *arch, "-O2", "-Wall", "-D__3DS__", f'-DPROBE_MODE_NAME="{name}"',
             f"-I{dkp / 'libctru/include'}", HERE / "source/main.c",
             "-specs=3dsx.specs", f"-L{dkp / 'libctru/lib'}", "-lctru", "-lm", "-o", elf])
        rsf = BUILD / f"probe-{name}.rsf"
        rsf.write_text(RSF.format(unique_id=unique_id, system_mode_ext=mode), encoding="ascii")
        cia = BUILD / f"brawl3ds-probe-{name}.cia"
        run([args.makerom, "-f", "cia", "-o", cia, "-rsf", rsf, "-target", "t", "-elf", elf, "-icon", smdh])
        print(f"built {cia} ({cia.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
