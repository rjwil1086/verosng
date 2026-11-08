<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Milestones

An application that Windows 10 22H2 x64 can run installs and runs here. That set is x64 programs and 32-bit programs on the amd64 boot. A driver, a graphics backend, more RAM, or more processors becomes the next pass when the application under test fails without it.

Milestones 0–3 are done. Milestone 5 is the in-tree driver foundation that landed. Milestone 6 made the amd64 live CD the daily boot. Milestone 7 is the x64 application surface that landed. A task stays in the tree only when that boot still reaches the desktop. A syscall task also reruns the probe. A Windows driver loads beside the current boot device before it becomes the boot device.

The open work follows this order. Milestone numbers stay fixed.

1. Milestone 8. The application syscalls.
2. Milestone 18, beside milestone 8. Native i386 is unsupported.
3. Milestone 11. The boot reports Windows 10 22H2.
4. Milestone 21. One x64 application's installer completes and the program runs.
5. Milestone 20. The next application, one at a time.
6. Milestone 19. WoW64, after milestone 21.
7. Milestones 13 and 14, when the application needs `opengl32` or `d3d11`.
8. Milestones 15 and 16, when that application still fails on the software renderer.

Milestones 9, 10, 12, and 17 stay written down and wait. The desktop that already appears is enough when an application can be started from it. Display scaling, a new shell, and antivirus filters wait until a program under test requires them.

## Milestone 0: Project Infrastructure

Goal: Make AI-assisted development sustainable.

Deliverables:

- Fork ReactOS
- CI/CD pipeline
- Automated VM boot testing
- Automated crash collection
- Cursor project rules
- Coding standards
- Compatibility dashboard

Success criteria:

```text
Commit
↓
Build
↓
Boot VM
↓
Run tests
↓
Publish results
```

No manual testing required.

## Milestone 1: Windows Trace Harness

Goal: Build your truth source.

Deliverables:

- QEMU instrumentation
- Syscall tracing
- NTSTATUS logging
- Registry access tracing
- Process creation tracing
- Thread creation tracing

Output:

```json
{
  "syscall": "NtCreateFile",
  "status": "STATUS_SUCCESS"
}
```

Success criteria:

- Notepad traces collected
- Explorer traces collected
- Office traces collected
- Browser traces collected

You should have millions of Windows 10 syscall records.

## Milestone 2: Differential Testing

Goal: Turn compatibility into a measurable number.

Deliverables:

- Replay traces
- Compare Windows vs ReactOS
- Automated reports

Example:

```text
NtOpenProcess
Windows: SUCCESS
ReactOS: ACCESS_DENIED

Mismatch
```

Success criteria:

A dashboard showing:

```text
Top 100 syscalls
Compatibility: 64%
```

## Milestone 3: NTDLL Modernization

Goal: Fix user-mode compatibility first.

Deliverables:

- Win10 syscall tables
- Win10 structures
- Modern NTDLL exports
- Loader improvements

Target:

```text
notepad++
7zip
git
python
```

should launch reliably.

## Milestone 4: Core NT Compatibility

Goal: The kernel managers an application and a driver both enter are present on the amd64 boot.

Focus:

- Object manager
- Memory manager
- Process manager
- Thread manager
- Security subsystem

Success criteria:

- Those managers run on the daily amd64 boot.
- The syscall probe remains the regression check.

Calls an application makes with real arguments are milestone 8. The headline percent on the zero-argument probe does not close that work.

## Milestone 5: Driver foundation

Goal: Boot the live CD from the in-tree storport stack.

What landed:

- IRP, PnP, power, and DMA paths that `storahci` uses
- The QEMU AHCI disk identified as `QEMU HARDDISK` through in-tree `storahci`

Retail Windows drivers are milestone 12.

## Milestone 6: amd64 boot

Goal: Make the amd64 boot the daily boot and the headline score.

Deliverables:

- An `ARCH=amd64` bootcd that the daily boot, the boot test, and crash collection launch
- `ARCH=amd64` is the default build, in `build-amd64`
- `qemu-system-x86_64` is that boot
- The plugin and syscall-diff rebuilt for `SYSCALL` and the x64 register ABI
- i386 still builds when the build is asked for it, and it is no longer the boot that runs by default

Export names stay the same.

Success criteria:

- The daily boot is the amd64 guest
- The top-100 probe is recaptured on that native path and is the headline score

A Windows driver returning from `DriverEntry` is milestone 12, and it waits until an application needs that device. The kernel still reports NT 5.2 until milestone 11. Milestone 18 is the later decision that an i386 CPU is not a supported boot.

## Milestone 7: x64 application surface

Goal: A Windows 10 22H2 x64 application loads and a window appears.

A task stays only when the amd64 live CD still reaches the desktop. Wine updates for the libraries these programs call stay part of this milestone.

Tasks:

- Boot the daily amd64 live CD and record the first missing import or crash for x64 `git --version`. Change nothing.
- Add the one export or API-set forwarder that the log names. Rebuild, reach the desktop, and run that command again.
- Repeat that export task until `git --version` prints its version.
- Do the same loop for `python -c "print(1)"`.
- Do the same loop for 7-Zip opening one archive.
- Do the same loop for Notepad++ opening one file.
- Update the Wine library that supplied the last missing piece, then boot again and rerun all four actions.

Success criteria:

- Each target starts on the amd64 live CD and performs one ordinary action.
- A 32-bit build is scored only for a program that has no x64 build. A 32-bit program that has to run is milestone 19.

## Milestone 8: Application syscalls

Goal: The syscalls those programs make agree with Windows 10 22H2 when called with the arguments the programs pass.

This is the gate. Milestone 18 can land beside it. A task stays only when the amd64 live CD still reaches the desktop. For each name, record the Windows 10 22H2 status and the live CD status for the probe's real-argument call, then change the kernel until those statuses match. Rerun the full probe before the next name. Each distinct fault inside one name is its own pass.

Tasks:

- `NtCreateFile`
- `NtWriteFile`
- `NtReadFile`
- `NtClose`
- `NtDeviceIoControlFile`
- `NtCreateThreadEx`
- `NtCreateUserProcess`

Success criteria:

- Each of those names matches the Windows 10 22H2 guest on the probe's real-argument call.
- The rest of the top-100 probe does not regress.

This milestone stays on the current boot. Physical memory above 8 GiB is milestone 9. Logical processors, Hyper-Threading, and AMD SMT are milestone 10. Both wait until an application requires them.

## Milestone 18: Native i386 unsupported

Goal: An i386 CPU is not a boot this project supports. Applications run on the amd64 boot.

This can land while milestone 8 is open.

What lands:

- The docs name `ARCH=amd64` as the only supported build.
- The daily boot, the boot test, crash collection, and the probe do not build or launch `ARCH=i386`.
- New code is amd64. An i386-only assembly file stays untouched until an amd64 change is in that file.

What stays out: Deleting the i386 sources, the HAL, FreeLoader, or the CRT paths. NTVDM stays out with them.

Success criteria:

- An i386 build failure does not block a commit whose amd64 live CD still reaches the desktop.
- The milestone 8 probe still runs on that amd64 guest.

## Milestone 11: Application version report

Goal: The amd64 boot reports Windows 10 22H2 to an application.

This follows milestone 8. It does not wait for more processors or for the 6 TB ceiling. This changes the runtime NT version only. The tree keeps compiling as `_WIN32_WINNT=0x502`. The product version stays 0.4.x. In-tree images keep their existing PE version stamp. A task stays only when the amd64 live CD still reaches the desktop.

Requirements:

- Milestones 7 and 8 are done for the programs under test. The paths an application takes after it sees version 10 already return.
- `GetVersion` and `GetVersionEx` for an image without a Windows 10 manifest still return 6.2. `RtlGetVersion` and `SharedUserData` report the real version.
- `KmWin2k3VersionLie` still forces 5.2.3790 for a driver that fails when the major version is above 5.

Tasks:

- Record the current `SharedUserData`, `RtlGetVersion`, `PsGetVersion`, and unmanifested `GetVersion` values from the live CD.
- Make `SharedUserData`, `RtlGetVersion`, and `PsGetVersion` report 10.0.19045. Boot to the desktop and rerun the four milestone 7 actions.
- Confirm an unmanifested `GetVersion` and `GetVersionEx` still return 6.2. Leave `CurrentVersion` at 6.3, `CurrentMajorVersionNumber` at 10, and `CurrentBuildNumber` at 19045.
- Confirm `KmWin2k3VersionLie` still reports 5.2.3790.
- Rerun the milestone 8 probe.

Outcome:

- `SharedUserData`, `RtlGetVersion`, and `PsGetVersion` agree on 10.0.19045.
- The milestone 7 programs still start.
- The milestone 8 statuses do not regress.
- A driver covered by the 5.2 shim still observes 5.2.3790.

## Milestone 21: Install one x64 application

Goal: One x64 application's installer completes, and the program it installed runs.

This follows milestone 11. The live CD's existing installer is the setup step. The result that matters is the application. A task stays only when the amd64 live CD still reaches the desktop.

Tasks:

- Install onto a second disk and boot that disk to the desktop.
- Run one x64 application's installer. Record the first missing import, crash, or syscall status. Change nothing in that first run.
- Add the one export or status fix the log names. Reach the desktop and run the installer again.
- Repeat until the installer completes and the installed program starts.
- Reboot from that disk. The program and the files it wrote are still there.

Success criteria:

- The installed program starts after reboot.
- The milestone 7 programs still start.
- The milestone 8 statuses do not regress.
- The desktop still appears.

## Milestone 20: The next application

Goal: Each following application installs and runs the same way.

This follows milestone 21. One application at a time. A browser is one application in this loop. The network stack is the fix that program needs, when the log names it. A task stays only when the amd64 live CD still reaches the desktop.

Tasks:

- Record the first missing import, crash, or syscall status. Change nothing in that first run.
- Fix the one thing the log names. Reach the desktop and run the program again.
- Repeat until its installer completes and the program performs one ordinary action.
- Each distinct fault inside that application is its own pass. Rerun the milestone 8 probe before the next application.

Success criteria:

- That action completes.
- The milestone 8 statuses do not regress.
- The desktop still appears.

After milestone 19, this loop includes 32-bit applications.

## Milestone 19: WoW64

Goal: A 32-bit program with no x64 build installs and completes one action on the amd64 system.

This follows milestone 21. It is a subsystem of the amd64 boot. An i386 CPU and NTVDM stay out. A program that refuses to run below version 10 uses the milestone 11 report. A task stays only when the amd64 live CD still reaches the desktop.

Tasks:

- Choose one 32-bit program that has no x64 build. Record the first missing import, crash, or syscall status. Change nothing in that first run.
- Fix the one thing the log names. Reach the desktop and run the program again.
- Repeat until its installer completes and the program performs one ordinary action.

Success criteria:

- That action completes on the installed amd64 system.
- The milestone 8 statuses do not regress.
- The desktop still appears.

## Milestone 13: Graphics HAL

Goal: One boundary owns pixel and GPU work so a 3D application can change backends.

Start this when the application under test needs `opengl32` or `d3d11`.

```text
user32
gdi32
dxgi
d3d11
opengl32

↓

Graphics HAL

↓

Software or virtual renderer
DRM
WDDM 2.0
```

Applications keep calling the Windows DLLs. Backend-specific code stays below the HAL. A task stays only when the amd64 live CD still reaches the desktop.

Tasks:

- Add the HAL interface and a software backend that calls the existing GDI path. The desktop pixels stay the same.
- Select that backend from one place. Boot to the desktop.
- Add a second backend id that fails and falls back to the software backend. Boot to the desktop.
- Open a milestone 7 window through the software backend.

Success criteria:

- The GDI desktop still shows a milestone 7 window through the software backend.
- A second backend can be selected without an application change.

## Milestone 14: First 3D backend

Goal: The application from milestone 13 renders through the Windows-facing DLLs on the software or virtual backend.

This follows milestone 13. The first renderer is llvmpipe or virtio-gpu. A task stays only when the amd64 live CD still reaches the desktop.

Tasks:

- Clear one surface to a solid color through the software backend and show it in a window.
- Draw one triangle through `opengl32` on llvmpipe or virtio-gpu.
- Draw one triangle through `d3d11` using DXVK, vkd3d-proton, or WineD3D.
- Run one 3D program and keep one frame on screen. Confirm the milestone 7 window still opens.

Success criteria:

- One 3D program draws a frame on the amd64 boot.
- The milestone 7 window still appears.

## Milestone 15: DRM backend

Goal: The same HAL gains a DRM backend, using a GPL Linux GPU driver as the hardware source, when the application still fails on the software renderer.

```text
Graphics HAL
↓
DRM backend
↓
One Linux GPU driver
```

Start this when the milestone 14 application still fails on llvmpipe or virtio-gpu. A task stays only when the amd64 live CD still reaches the desktop. The software backend stays selected until the real GPU has already drawn a frame.

Tasks:

- Add the DRM backend and fall back to software when it cannot start. Boot to the desktop.
- Modeset one simple QEMU display device through that backend and show the desktop.
- Draw the milestone 14 frame on that device.
- Bring up one real Linux GPU driver on one machine, with the software backend still selected by default.
- Draw the milestone 14 frame on that GPU, then make that backend the selected one.

Success criteria:

- The milestone 14 program draws a frame on that GPU.
- Selecting the DRM backend does not change the application or the DLL exports.

## Milestone 16: WDDM 2.0 backend

Goal: The same HAL gains a WDDM 2.0 backend for an application that still fails on the DRM backend.

```text
dxgkrnl
dxgmms2
D3DKMT entry points
```

mapped internally to the HAL. This follows milestone 15. A task stays only when the amd64 live CD still reaches the desktop. The previous backend stays the boot display until this backend has already presented a frame.

Tasks:

- Add the WDDM backend and fall back to the current backend when it cannot start. Boot to the desktop.
- Make one `D3DKMT` query return the Windows status for the probe's arguments.
- Present the milestone 14 frame through this backend inside QEMU.
- Load one unmodified Windows GPU driver while the previous backend is still the boot display.
- Present a frame with that driver, then make the WDDM backend the selected display.

Success criteria:

- The milestone 14 program draws a frame through this backend.
- An unmodified Windows GPU driver is the last step, after the backend's own entry points return.

## Milestone 9: amd64 physical memory

Goal: The amd64 boot keeps physical RAM up to the Windows 10 22H2 maximum of 6 TB.

Windows 10 22H2 documents 128 GB for Home, 2 TB for Pro and Education, and 6 TB for Enterprise and Pro for Workstations. There is no edition split here, so the ceiling is 6 TB. A larger machine still boots, and memory past that ceiling is omitted.

FreeLoader keeps its own 1 GiB map. The kernel receives every firmware range up to 6 TB. The PFN database is followed immediately by nonpaged pool, and that pool ends before the HAL. A 4 KB page frame holds 36 bits, so a page above 1 TB can be named. The write and dirty bits stay where they are. The nonpaged pool ceiling stays 128 GB.

Milestones 9, 10, 12, and 17 stay off the main list until the application under test fails for lack of that resource or device. A task stays only when the amd64 live CD still reaches the desktop. This lands before more processors start, when both this milestone and milestone 10 are underway, so the page tables are settled on one processor.

Tasks:

- Size the FreeLoader page lookup table for the 1 GiB the loader maps. Walk every firmware descriptor above that and pass those pages to the kernel. Clip the list at 6 TB.
- Place nonpaged pool at the first byte after the PFN database.
- Widen the amd64 page-frame fields so a page above 1 TB can be named.
- Boot the live CD at 8 GiB and at 16 GiB to the desktop. Boot again at the largest size this host can give QEMU.

Success criteria:

- 8 GiB and 16 GiB both reach the desktop, and the reported RAM matches the guest aside from firmware holes.
- The kernel accepts a physical page above 8 GiB and below 6 TB.
- A guest larger than 6 TB boots with the first 6 TB.
- The milestone 8 statuses do not regress.

## Milestone 10: amd64 logical processors

Goal: The amd64 boot starts every logical processor, and Hyper-Threading and AMD SMT behave as they do on Windows 10 22H2 for a machine that fits in one processor group.

This does not wait for the 6 TB ceiling. If milestone 9 has already widened the page frames, the TLB shootdown uses that layout.

One group holds at most 64 logical processors. The KTHREAD and KPRCB layouts stay. Processor groups, multi-node NUMA, hybrid efficiency classes, and core parking stay out. A machine with more than 64 logical processors keeps the first 64.

The live entries switch to `ntkrnlmp.exe` and `halmp.dll` only after a 2-CPU boot has reached the desktop. `ONECPU` stays the single-processor escape. The uniprocessor images stay on the CD.

Tasks:

- Start each application processor. Real mode enters long mode, then `KiSystemStartup`. Wait with a timeout. `ONECPU` starts only the boot processor.
- Shoot the TLB down with an IPI. Implement `KeIpiGenericCall`. Deliver IPIs by physical APIC ID past 8 processors. Update a running thread's cycle time from the CPU it is on.
- Read Intel leaves `0x1F` or `0xB`, and AMD leaves `80000008` and `8000001E`. A core with one logical processor is not SMT. A core with two is SMT for both vendors. Fill `MultiThreadProcessorSet`. Use x2APIC when an APIC ID does not fit in xAPIC.
- When two threads can run, place them on different cores before they share a Hyper-Thread or an AMD SMT sibling.
- Report that topology from `GetLogicalProcessorInformationEx`: the logical count, one core record per core with the sibling mask, packages, caches, one NUMA node, and group 0. Die and module records appear only when CPUID reports those levels.
- Point the live CD at `ntkrnlmp.exe` and `halmp.dll`. Rerun the milestone 8 probe.

Success criteria:

- `-smp 4,sockets=1,cores=4,threads=1` reports 4 logical processors and 4 cores.
- `-cpu qemu64 -smp 4,sockets=1,cores=2,threads=2` reports 4 logical processors and 2 cores, each mask covering a pair.
- `-cpu EPYC -smp 4,sockets=1,cores=2,threads=2` reports that same shape from the AMD leaves.
- Two runnable threads prefer two cores over two siblings of one core.
- The desktop still appears, and the milestone 8 statuses do not regress.
- `ONECPU` still boots on the boot processor.

## Milestone 12: Windows storage, network, and USB drivers

Goal: A retail Windows 10 22H2 driver for non-graphics hardware loads on the amd64 boot when the application under test needs that device.

Run the task loop for one driver at a time. A disk driver attaches to a second disk. The live CD keeps booting from the in-tree driver until the boot task for that driver.

Order:

- Intel NIC
- Realtek NIC
- USB controller
- SATA/AHCI
- NVMe

Tasks:

- Load the Windows driver without replacing the boot device. Record the first bugcheck, or the status `DriverEntry` returns.
- Make the one export named in that log return. Boot to the desktop and load the driver again.
- Repeat that export task until `DriverEntry` returns.
- Complete one ordinary operation: a packet, a USB enumeration, or a read from the second disk.
- For SATA/AHCI or NVMe only, boot from that driver after the ordinary operation has already succeeded on the second disk.

Success criteria:

- The driver's `DriverEntry` returns.
- The exports it calls into `ntoskrnl`, the HAL, and KMDF return.
- The device it owns performs its ordinary operation.
- A driver that rejects a version above 5 still runs under `KmWin2k3VersionLie`.
- A driver that requires version 10 waits until milestone 11, then passes that check and still returns.

## Milestone 17: Remaining non-graphics drivers

Goal: Extend the same bar past boot and network hardware when the application under test needs that device.

Run the milestone 12 task loop for each device below. The device stays off the boot path. Milestones 13 through 16 do not wait for this milestone.

Tasks:

- One audio driver
- One Bluetooth driver
- One printer driver
- One storage filter

Success criteria:

- Each chosen driver returns from `DriverEntry`, its kernel calls return, and the device performs its ordinary operation.

## What should the compatibility dashboard look like?

I'd make the dashboard the center of the whole project. The goal is not to be pretty. The goal is to answer:

"What compatibility improved today, what broke, and where should Cursor work next?"

The percentages in this section are a design specimen. They are not a measured score. The measured score is the amd64 probe snapshot.

### Top-Level Overview

```text
===================================================
ReactOS 10 Compatibility Dashboard
===================================================

Overall Compatibility Score: 67.3%

Windows Build Target:
Win10 22H2 x64

Applications:
✅ 412 Working
⚠️ 168 Partial
❌ 921 Failing

Drivers:
✅ 84 Working
⚠️ 31 Partial
❌ 210 Failing

Kernel ABI:
78.4%

Last Build:
#1832
```

### Syscall Compatibility

This would be the most important page.

```text
TOP SYSCALLS

NtCreateFile 98.2%
NtOpenFile 99.1%
NtCreateSection 72.3%
NtMapViewOfSection 69.8%
NtCreateThreadEx 91.1%
NtOpenProcess 95.6%
NtQueryInformationProcess 81.4%

Overall:
87.2%
```

Clicking one:

```text
NtCreateSection

Compatibility:
72.3%

Windows Calls:
1,925,442

Matches:
1,391,987

Mismatches:
533,455
```

### Divergence Explorer

This is where the real value lives.

```text
NtCreateSection

Most Common Differences:

1.
Windows:
STATUS_OBJECT_NAME_NOT_FOUND

ReactOS:
STATUS_NOT_IMPLEMENTED

Count:
182,112

2.
Windows:
STATUS_SUCCESS

ReactOS:
STATUS_ACCESS_DENIED

Count:
56,221
```

Now Cursor gets handed a concrete task.

### Application Compatibility

A giant matrix.

```text
Applications

7-Zip ✅
Firefox ✅
Chrome ⚠️
Steam ⚠️
Discord ✅
VS Code ✅
Office 365 ❌
Photoshop ❌
```

Clicking Chrome:

```text
Chrome

Launches:
Yes

Browse:
Yes

GPU:
No

Crash after 30m:
Yes

Compatibility:
74%
```

### Driver Compatibility

One of your most valuable pages.

```text
Network Drivers

Intel I219-V ✅
Realtek RTL8111 ✅
Broadcom BCM57780 ⚠️

Storage Drivers

Samsung NVMe ✅
Intel AHCI ✅
AMD SATA ⚠️

Audio

Realtek HD Audio ⚠️
Intel HD Audio ✅
```

### Regression Tracker

Must-have feature.

```text
Last Build:

Overall:
66.8%

Current Build:

Overall:
67.3%

Change:
+0.5%
```

And:

```text
Improvements

NtOpenProcess +8%
NtCreateSection +11%

Regressions

NtMapViewOfSection -1.2%
```

Nothing kills a compatibility project faster than accidentally breaking old fixes.

### Heat Map

Let data drive development.

```text
Subsystems

Memory Manager 61%
Object Manager 92%
Registry 96%
Security 78%
Processes 85%
Threads 87%
Filesystem 91%
Graphics 21%

Highest Impact Area:
Graphics
```

This tells you exactly where effort belongs.

### Trace Corpus

Show what real software actually uses.

```text
Observed Syscalls

NtCreateFile 17%
NtQueryInformationProcess 12%
NtMapViewOfSection 9%
NtOpenKey 8%
NtQueryValueKey 7%
```

This prevents wasting time on obscure APIs.

If some syscall appears in 0.000001% of traces, don't spend a month on it.

### AI Task Generator

This is where your Cursor workflow becomes powerful.

Example:

```text
Highest Value Fix

Subsystem:
Memory Manager

Function:
NtCreateSection

Mismatch Count:
533,455

Affected Applications:
Chrome
Office
Steam

Estimated Compatibility Gain:
+2.4%
```

Then automatically generate:

```text
TASK #493

Compare Windows and ReactOS
NtCreateSection behavior.

Attached:
- Windows traces
- ReactOS traces
- Existing implementation
- Wine implementation

Goal:
Reduce STATUS_NOT_IMPLEMENTED mismatches.
```

Cursor gets fed these tasks all day.

### Driver ABI Score

Since driver support is one of your goals:

```text
Driver ABI Support

IRP Handling 93%
PnP Manager 68%
Power Manager 52%
DMA 47%
KMDF 39%
UMDF 14%
```

This would tell you much more than "drivers don't work".

### Graphics Page

Eventually:

```text
Graphics

Software Backend:
✅

DRM Backend:
✅

Compositor:
✅

DXVK:
⚠️

D3D11:
47%

D3D12:
12%

WDDM Compatibility:
5%
```

### Compatibility Score Formula

I would not weight everything equally.

Maybe:

```text
40% Syscall Compatibility

25% Application Compatibility

20% Driver Compatibility

10% Graphics Compatibility

5% Test Pass Rate
```

Otherwise you'll game the numbers by fixing obscure APIs nobody uses.

### Single Most Important Metric

If I could only keep one graph, it would be:

```text
Top 100 Most Used Syscalls

Current:
83.2%

Last Week:
81.4%

Last Month:
77.5%
```

Because if those top 100 APIs become Windows-accurate, you'll see improvements everywhere:

- More applications launch
- More drivers load
- Fewer crashes occur
- Cursor has clearer guidance

That one dashboard would effectively become the "North Star" metric for the entire project.
