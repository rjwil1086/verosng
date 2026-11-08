# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(verreport verreport.c)
set_module_type(verreport win32gui UNICODE)
add_importlibs(verreport advapi32 ntdll msvcrt kernel32)
add_cd_file(TARGET verreport DESTINATION reactos/system32 FOR all)
