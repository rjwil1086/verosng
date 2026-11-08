<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Milestones

Milestones 0–5 are done. The daily boot is amd64. Milestone 7 makes that boot report Windows 10. Milestone 8 moves the applications and the in-tree drivers onto it.

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

Goal: Stabilize kernel behavior.

Focus:

- Object manager
- Memory manager
- Process manager
- Thread manager
- Security subsystem

Success criteria:

Top 100 syscalls pass compatibility testing at 80-90%.

## Milestone 5: Driver Compatibility Foundation

Goal: Load modern Windows drivers.

Focus:

- IRP handling
- PnP manager
- Power manager
- DMA support

Target drivers:

```text
Intel NIC
Realtek NIC
USB controllers
SATA/AHCI
NVMe
```

Success criteria:

Common hardware works using Windows 10 drivers.

## Milestone 6: amd64 boot

Goal: Make the amd64 boot the daily boot and the headline score.

This starts when a WDK driver returns from `DriverEntry` on the i386 boot and the next failures are pointer width, structure size, or the x64 calling convention. An x64-only driver, such as NVMe or most NICs on the milestone 5 list, is the other trigger.

The kernel still reports NT 5.2 through this milestone. Milestone 7 changes that report.

Deliverables:

- An `ARCH=amd64` bootcd that the daily boot, the boot test, and crash collection launch
- `ARCH=amd64` is the default build, in `build-amd64`
- `qemu-system-x86_64` is that boot
- The plugin and syscall-diff rebuilt for `SYSCALL` and the x64 register ABI
- i386 still builds when the build is asked for it, and it is no longer the boot that runs by default

Export names stay the same.

Success criteria:

- The daily boot is the amd64 guest
- A Windows 10 22H2 WDK amd64 driver loads and `DriverEntry` returns while the kernel still reports 5.2.3790
- The top-100 probe is recaptured on that native path and is the headline score

## Milestone 7: Windows 10 version report

Goal: The default amd64 boot reports Windows 10 22H2.

This changes the runtime NT version only. The tree keeps compiling as `_WIN32_WINNT=0x502`. The ReactOS product version stays 0.4.x. In-tree images keep their existing PE version stamp.

Requirements:

- Milestone 6 is done. The same WDK driver has returned from `DriverEntry` while the kernel still reports 5.2.3790. That boot is the before measurement.
- The exports that driver calls after it accepts version 10 already return.
- `GetVersion` and `GetVersionEx` for an image without a Windows 10 manifest still return 6.2. `RtlGetVersion` and `SharedUserData` report the real version.
- `KmWin2k3VersionLie` still forces 5.2.3790 for a driver that fails when the major version is above 5.

Outcome:

- `SharedUserData`, `RtlGetVersion`, and `PsGetVersion` agree on 10.0.19045.
- `CurrentVersion` stays `6.3`. `CurrentMajorVersionNumber` is 10 and `CurrentBuildNumber` is 19045.
- That WDK driver passes its version check and `DriverEntry` still returns.
- A driver covered by the 5.2 shim still observes 5.2.3790.
- The top-100 probe does not regress.

## Milestone 8: 64-bit migration

Goal: Make the Windows 10 22H2 x64 ABI the project default beyond the boot.

Milestone 6 makes the amd64 boot the daily boot. Milestone 7 makes that boot report Windows 10. This milestone moves the rest of the work onto it: the build, the applications, and the drivers already proven on i386.

Deliverables:

- CI and the compatibility snapshot use the amd64 boot
- The milestone 3 applications run as 64-bit binaries
- In-tree drivers that loaded on the i386 live CD load on the amd64 boot
- i386 still builds, and it is not the headline score

Success criteria:

- The apps and those in-tree drivers are no longer scored on i386

## Milestone 9: Graphics HAL (GKAL)

Goal: Future-proof graphics.

Create:

```text
Win32
DXGI
DirectX
GDI

↓

GKAL

↓

Backend
```

No backend-specific code allowed above GKAL.

Success criteria:

Window manager renders through GKAL.

## Milestone 10: Software Renderer

Goal: Reliable graphics platform.

Implement:

```text
CPU Rendering
Framebuffers
Compositor
Desktop
```

Benefits:

- Easy debugging
- Deterministic tests
- Runs everywhere

## Milestone 11: Mesa / DRM Backend

Goal: Hardware acceleration quickly.

Implement:

```text
GKAL
↓
Vulkan
↓
Mesa
↓
Linux GPU Drivers
```

Success criteria:

Desktop composition accelerated.

## Milestone 12: Modern Desktop

Goal: Make daily usage possible.

Features:

- DWM-style compositor
- Modern explorer shell
- Multi-monitor support
- DPI scaling
- Hardware accelerated rendering

Success criteria:

Can use the OS daily.

## Milestone 13: Wine Synchronization Layer

Goal: Continuously ingest Wine improvements.

Build tooling:

```text
Wine
↓
Analysis
↓
Suggested merge
↓
AI review
```

Cursor can continuously evaluate Wine commits.

## Milestone 14: DirectX Compatibility

Leverage:

```text
DXVK
vkd3d-proton
WineD3D
```

Path:

```text
D3D9
D3D10
D3D11
```

Target:

Steam games start rendering correctly.

## Milestone 15: Driver Compatibility Expansion

Focus on:

- Audio
- Bluetooth
- Printers
- Storage filters
- Antivirus filters

Success criteria:

Large numbers of retail Windows 10 drivers load successfully.

## Milestone 16: WDDM Compatibility Layer

Only start this after everything above is stable.

Implement:

```text
dxgkrnl
dxgmms
D3DKMT entry points
```

mapped internally to GKAL.

This lets applications think they're talking to WDDM.

## Milestone 17: Native WDDM Drivers (Optional)

This is the moonshot.

Goal:

```text
Windows NVIDIA Driver
Windows AMD Driver
Windows Intel Driver
```

load without modification.

This is likely the most difficult milestone in the entire project.

## What I'd Focus On First Year

If it were my project, Year 1 would end with:

- Trace harness
- Differential testing
- Compatibility dashboard
- Modernized NTDLL
- Improved kernel ABI compatibility
- Driver compatibility foundations
- GKAL architecture

I would not spend Year 1 touching WDDM.

Milestone 6 makes the amd64 boot the daily boot and the default build. Milestone 7 reports Windows 10 from that boot. Milestone 8 moves the applications and the in-tree drivers onto it before graphics work. None of these is a reason to start WDDM in Year 1.

The biggest force multiplier is getting to this workflow:

```text
Windows Trace
↓
Diff Engine
↓
Cursor Fix
↓
Regression Test
↓
Compatibility Score Increase
```

Once that exists, every future milestone becomes dramatically easier because you're improving compatibility with data instead of intuition.

## What should the compatibility dashboard look like?

I'd make the dashboard the center of the whole project. The goal is not to be pretty. The goal is to answer:

"What compatibility improved today, what broke, and where should Cursor work next?"

The percentages in this section are a design specimen. They are not a measured score. The measured score stays the snapshot from the i386 probe until milestone 6 makes the amd64 boot the daily boot.

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
