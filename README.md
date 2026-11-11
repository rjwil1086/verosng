# VerOS NT™

VerOS NT is Ryan Wilson's hobby project: a one-person fork of [ReactOS](https://github.com/reactos/reactos). The goal is a general-purpose operating system that can install and run any application Windows 10 22H2 can install and run. That set is x64 programs and, later, 32-bit programs on the amd64 boot.

These are alpha builds for the harness, experiments, and watching compatibility scores move. Run them in a virtual machine. A daily machine or a production environment needs a finished OS.

Cursor and Grok write most of the patches. Ryan reviews them before they land. Notes under [`veros/transparency/`](veros/transparency/) record the sources for a Windows-behavior change. A prompt may use QEMU harness traces, public documentation, this tree under GPL-2.0, and Wine code under the license already on the file. Leaked or proprietary Windows source, and decompiled or disassembled Microsoft binaries, stay out of the notes and out of the prompts.

VerOS NT is a trademark of Ryan Wilson. Windows is a registered trademark of Microsoft Corporation. Microsoft and the ReactOS Project have not licensed or endorsed this fork.

## Plan

The path is the [milestones on GitHub](https://github.com/rjwil1086/verosng/milestones). Open milestones follow the number in the title. Close a milestone there when the work is done. Rename the title to change its number.

An application that Windows 10 22H2 x64 can run installs and runs here. A driver, a graphics driver, more RAM, or more processors becomes the next pass when the application under test fails without it. Display scaling, a new shell, and antivirus filters wait until a program under test requires them.

Finished milestones keep their numbers. A later milestone is entered early only when the log from the milestone in progress names its missing piece, and the work then returns to that milestone. A missing Windows GPU driver does not interrupt milestones 29 through 33.

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
