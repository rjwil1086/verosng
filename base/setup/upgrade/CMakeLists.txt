add_executable(upgrade upgrade.c)
set_module_type(upgrade win32gui UNICODE)
add_importlibs(upgrade user32 comctl32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET upgrade DESTINATION VerOS NO_CAB FOR bootcd)
