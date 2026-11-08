# Fork coding standards

C and C++ in this tree follow [CODING_STYLE.md](../CODING_STYLE.md), [.clang-format](../.clang-format), and the commit guidance in [CONTRIBUTING.md](../CONTRIBUTING.md). Do not keep a second C style. Wine-synced files in `media/doc/WINESYNC.txt` and third-party files in `media/doc/3rd Party Files.txt` are not reformatted.

The project goal is that an application Windows 10 22H2 x64 can run installs and runs. A WDK-built driver keeps its calling convention, and the kernel services the exports it already calls. The roadmap is [MILESTONES.md](MILESTONES.md).

## Open source

Files added in this fork are GPL-2.0-compatible and carry an SPDX identifier. The QEMU plugin is GPL-2.0-or-later. Third-party code has to be GPL-2.0-compatible and listed with the other third-party files. Generated files keep their source in the tree.

## Clean room

Windows behavior is reimplemented from public documentation and from logs of this guest. Windows source, leaked NT source, the Windows Research Kernel, decompiler output, disassembly of Microsoft binaries, WDK headers, libraries, and `.sys` files are not inputs and are not committed. A structure-offset check may record numbers and field names only.

## Pipeline, harness, and dashboard

PowerShell scripts in `tools/` and the static files in `dashboard/` use 4-space indentation and an SPDX header. Compatibility rows in `compat/` stay `untested` until a spec line, a path, a plugin event, or a serial log supports a status. A stub is not a pass. A plugin return without a bugcheck is what marks an export working.
