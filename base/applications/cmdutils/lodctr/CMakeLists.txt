
add_executable(lodctr lodctr_main.c)
set_module_type(lodctr win32cui UNICODE)
add_importlibs(lodctr loadperf msvcrt kernel32)
add_cd_file(TARGET lodctr DESTINATION VerOS/system32 FOR all)
