
add_executable(screenshot screenshot.c screenshot.rc)
set_module_type(screenshot win32gui UNICODE)
add_importlibs(screenshot user32 gdi32 comdlg32 msvcrt kernel32)
add_cd_file(TARGET screenshot DESTINATION VerOS/system32 FOR all)
