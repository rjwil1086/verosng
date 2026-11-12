# Windows 10 syscall traces

This is the truth source for Windows 10 22H2 x64 call behavior. The traces are produced by booting a guest and are not committed. After a run, the result files are copied into a private archive outside this repository, one new folder each time.

## Tools

- Install and capture use `C:\Program Files\qemu\qemu-system-x86_64.exe`.
- `qemu-system-x86_64.exe` is the VerOS boot binary. The daily boot passes `-accel tcg`.
- The x86_64 binary has WHPX and TCG. The guest is the powered-off VMware UEFI VM `D:\VMware\Windows 10 x64\Windows 10 x64.vmdk`. Capture opens that VMDK directly with OVMF, NVMe, and `-accel tcg -snapshot -m 8192 -smp 4`. The plugin instruments TCG only. The VerOS NT boot uses the same memory and CPU count.

## Commands

```powershell
.\tools\qemu-harness\Test-Plugin.ps1
.\tools\windows-trace\Install-Windows.ps1
.\tools\windows-trace\Collect-Traces.ps1
.\tools\windows-trace\Test-TraceCorpus.ps1
```

Defaults on this machine:

- ISO: `D:\ISO\Windows_10_22H2_English_x64.iso`
- Office: `C:\Users\RyanWilson\Downloads\LibreOffice_26.2.1_Win_x86-64.msi`
- Browser: `C:\Users\RyanWilson\Downloads\Firefox Installer.exe`
- Guest agent and the virtio serial driver: `D:\ISO\virtio-win-0.1.271.iso`

Logs are `output/windows-trace/*.jsonl`. The capture disk is the VMware VMDK. `-snapshot` keeps those trace writes off the VMware file.

Each JSON line has `syscall` and `status`. A Windows line is recorded only when the bytes are the public stub `mov r10, rcx` / `mov eax, imm` / `test` / `jnz` / `syscall`, or that pair of moves directly before `syscall` (or the `INT 2E` form used by the fixture). `n` is that immediate. `syscall` is that number unless the plugin was given a caller-supplied map. This repo does not carry a Windows ordinal table. When that stub is the start of a translation block, `a1` through `a4` are `rcx`, `rdx`, `r8`, and `r9` at the stub. `status` uses the short public list in `tools/windows-trace/ntstatus.txt`. A registry or file call adds `path` when that map names the call and the public `OBJECT_ATTRIBUTES` / `UNICODE_STRING` layout can be read.

`{"phase":"workload"}` is written once the capture script starts the program, so the lines after it are the workload and the lines before it are the boot.
