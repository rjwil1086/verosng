# QEMU accelerator patch

`abi.dll` is an out-of-tree TCG plugin loaded with `-plugin` by the installed QEMU. The QEMU source tree is not vendored here.

If logging `SYSENTER`, `SYSCALL`, `INT 2E`, and matched `CALL`/`JMP` instructions makes a TCG boot too slow to reach the ReactOS version banner, put a patch series against QEMU 11.1.0 `target/i386` in this directory. The patch should emit only those events. Do not copy Windows source or decompiler output into the patch.
