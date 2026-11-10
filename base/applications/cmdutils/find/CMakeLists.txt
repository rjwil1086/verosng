
add_executable(find find.c find.rc)
set_module_type(find win32cui UNICODE)
target_link_libraries(find conutils)
add_importlibs(find msvcrt kernel32)
add_cd_file(TARGET find DESTINATION VerOS/system32 FOR all)
