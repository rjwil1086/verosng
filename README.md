# VerOS NT

VerOS NT is Ryan Wilson's hobby project: a one-person fork of [ReactOS](https://github.com/reactos/reactos). The goal is a general-purpose operating system that can install and run any application Windows 10 22H2 can install and run. That set is x64 programs and, later, 32-bit programs on the amd64 boot.

These are alpha builds for the harness, experiments, and watching compatibility scores move. Run them in a virtual machine. A daily machine or a production environment needs a finished OS.

Cursor and Grok write most of the patches. Ryan reviews them before they land. Each generated change ships with a public note under [`veros/transparency/`](veros/transparency/). A prompt may use QEMU harness traces, public documentation, this tree under GPL-2.0, and Wine code under the license already on the file. Leaked or proprietary Windows source, and decompiled or disassembled Microsoft binaries, stay out of the notes and out of the prompts.

Windows is a registered trademark of Microsoft Corporation. Microsoft and the ReactOS Project have not licensed or endorsed this fork.

## Plan

The path is [docs/MILESTONES.md](docs/MILESTONES.md). A commit note marks a milestone in progress when it says the work is for that milestone, and complete when it says that milestone is done. A transparency record does the same when it says that milestone is done.

An application that Windows 10 22H2 x64 can run installs and runs here. A driver, a graphics driver, more RAM, or more processors becomes the next pass when the application under test fails without it. Display scaling, a new shell, and antivirus filters wait until a program under test requires them.

Finished milestones keep their numbers. The open work follows this order. A later milestone is entered early only when the log from the milestone in progress names its missing piece, and the work then returns to that milestone.

1. Milestone 9. Native i386 is unsupported.
2. Milestone 13. One module compiles as Windows 10 22H2 when that installer's log names a prototype `0x502` hides. The installer then runs again.
3. Milestone 14. Service numbers, when a program issues a direct call and the number disagrees with the name list.
4. Milestone 15. The next application, one at a time.
5. Milestone 16. WoW64, after the first installed x64 program. Milestone 15 then includes 32-bit programs.
6. Milestone 17. A retail driver, when the application needs that device.
7. Milestone 18. One shared structure layout, when milestone 17 or 15 names a wrong size or offset.
8. Milestone 19. The remaining non-graphics drivers, after milestone 17, when the application needs that device.
9. Milestone 20, when the application needs `opengl32` or `d3d11`.
10. Milestone 21, when that application fails on the software rasterizer.
11. Milestone 22, when that application still fails on virtio-gpu.
12. Milestone 23, when that application still fails on the DRM driver.
13. Milestone 24. Physical memory above 8 GiB, when an application fails for lack of RAM.
14. Milestone 25. Logical processors, when an application needs them.
15. Milestone 26. The Vista page-table software bits, after milestone 24.
16. Milestone 27. The root compile becomes Windows 10 22H2, after milestones 24, 13, and 18.

## Compatibility loop

The same QEMU harness path runs a reference guest and a VerOS NT guest.

1. The harness records traces, boot results, and crashes from both guests.
2. The diff engine places reference behavior beside the VerOS NT result.
3. Cursor and Grok get a concrete mismatch, and the generation record is published.
4. The harness boots again, checks for a crash, and runs the regression test.
5. Compatibility moves because the harness measured it.

`ARCH=amd64` is the only supported OS build. A native i386 boot is unsupported. 32-bit applications run on the amd64 boot.

## Building

The daily build is Windows, Clang 18.1.8, amd64, Debug, target `bootcd`. `ARCH=amd64` is the only supported OS build. Details and tool paths are in [docs/LOCAL_BUILD.md](docs/LOCAL_BUILD.md).

From the source tree:

```powershell
.\tools\pipeline\Build-Bootcd.ps1
.\tools\pipeline\Test-Boot.ps1
```

`Build-Bootcd.ps1` writes `build-amd64\bootcd.iso`. It uses RosBE 2.2.1 and Clang 18.1.8 from the parent `tools` folder when `Setup-BuildEnvironment.ps1` created them, and otherwise from `D:\RosBE`, `C:\RosBE`, or `D:\LLVM-18`. `Test-Boot.ps1` boots that ISO with QEMU 11.1.0, 8 GiB of RAM, 4 processors, and TCG. `build-amd64/` and `output/` stay untracked.

## License

GPL-2.0 is the default. The text is in [COPYING](COPYING). A file header wins when it names a different license. Wine-synced code keeps the license it arrived with.

Upstream is [reactos/reactos](https://github.com/reactos/reactos). Development of this fork is on [rjwil1086/verosng](https://github.com/rjwil1086/verosng).
