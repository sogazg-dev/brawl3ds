"""Package the headless timing build: a 178 MB-mode CIA plus the SD card folder it needs.

Usage: python -I tools/package_headless.py --build <headless-build dir> --wiicompiled <wiicompiled dir>
           --blobs <staged blobs dir> --out <output dir> [--makerom PATH] [--devkitpro C:/devkitPro]

Output:
  <out>/brawl3ds-headless.cia          install with FBI (178 MB system mode, 804 MHz, L2 cache)
  <out>/brawl3ds-headless.3dsx         the same program for the Homebrew Launcher (gets the launcher's memory, not 178 MB)
  <out>/sd/3ds/brawl3ds/               copy to the SD card root's 3ds folder, then add game/ (the extracted disc)
      blobs/ wii_bootstrap/ dsp_coef.bin portable.txt
No game data is written here except the section images the user's own translation produced (blobs/).
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

RSF = """BasicInfo:
  Title: Brawl3DS headless
  ProductCode: CTR-P-B3DH
  Logo: Homebrew
TitleInfo:
  Category: Application
  UniqueId: 0xBB3D2
Option:
  UseOnSD: true
  FreeProductCode: true
  MediaFootPadding: false
  EnableCrypt: false
  # The ~70 MiB code segment is stored uncompressed: the exefs .code compression (BLZ) is meant for small
  # executables, and a compressed image this size came out of the loader corrupted.
  EnableCompress: false
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
  SystemModeExt: 178MB
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
    GetProcessIdealProcessor: 6
    GetProcessInfo: 43
    GetResourceLimit: 56
    GetResourceLimitCurrentValues: 58
    GetResourceLimitLimitValues: 57
    GetSystemInfo: 42
    GetSystemTick: 40
    GetThreadContext: 59
    GetThreadId: 55
    GetThreadIdealProcessor: 15
    GetThreadInfo: 44
    GetThreadPriority: 11
    MapMemoryBlock: 31
    OutputDebugString: 61
    QueryMemory: 2
    ReleaseMutex: 20
    ReleaseSemaphore: 22
    SendSyncRequest1: 46
    SendSyncRequest2: 47
    SendSyncRequest3: 48
    SendSyncRequest4: 49
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
   - dsp::DSP
   - fs:USER
   - gsp::Gpu
   - hid:USER
   - ir:rst
   - ptm:u
SystemControlInfo:
  SaveDataSize: 0KB
  RemasterVersion: 0
  StackSize: 0x100000
  Dependency:
    cfg: 0x0004013000001702
    dsp: 0x0004013000001a02
    gsp: 0x0004013000001c02
    hid: 0x0004013000001d02
    ir: 0x0004013000003302
    ptm: 0x0004013000002202
"""


def run(cmd):
    print(">", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--wiicompiled", type=Path, required=True)
    ap.add_argument("--blobs", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--devkitpro", type=Path, default=Path("C:/devkitPro"))
    ap.add_argument("--makerom", type=Path, default=Path.home() / "brawl3ds-work/tools/makerom/makerom.exe")
    args = ap.parse_args()

    elf = args.build / "brawl3ds.elf"
    for needed in (elf, args.blobs, args.makerom, args.wiicompiled / "native-build" / "wii_bootstrap",
                   args.wiicompiled / "native-build" / "dsp_coef.bin"):
        if not Path(needed).exists():
            sys.exit(f"missing: {needed}")
    args.out.mkdir(parents=True, exist_ok=True)

    # Icon: reuse the probe's generated PNG writer for a recognisable placeholder.
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "probes" / "memprobe"))
    from build_probe import png, icon_pixel  # noqa: E402
    icon_png = args.out / "icon.png"
    icon_png.write_bytes(png(48, 48, icon_pixel))
    smdh = args.out / "icon.smdh"
    run([args.devkitpro / "tools/bin/smdhtool.exe", "--create", "Brawl3DS headless",
         "Brawl timing test (no graphics)", "brawl3ds", icon_png, smdh])

    rsf = args.out / "brawl3ds-headless.rsf"
    rsf.write_text(RSF, encoding="ascii")
    cia = args.out / "brawl3ds-headless.cia"
    run([args.makerom, "-f", "cia", "-o", cia, "-rsf", rsf, "-target", "t", "-elf", elf, "-icon", smdh])
    shutil.copyfile(args.build / "brawl3ds.3dsx", args.out / "brawl3ds-headless.3dsx")

    sd = args.out / "sd" / "3ds" / "brawl3ds"
    sd.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.blobs, sd / "blobs", dirs_exist_ok=True)
    shutil.copytree(args.wiicompiled / "native-build" / "wii_bootstrap", sd / "wii_bootstrap", dirs_exist_ok=True)
    shutil.copyfile(args.wiicompiled / "native-build" / "dsp_coef.bin", sd / "dsp_coef.bin")
    (sd / "portable.txt").write_text("brawl3ds: settings, saves and logs stay in the UserData folder here.\n")
    print(f"\nCIA: {cia} ({cia.stat().st_size / 2**20:.1f} MiB)\nSD folder: {sd}  (add game/ = the extracted disc)")


if __name__ == "__main__":
    main()
