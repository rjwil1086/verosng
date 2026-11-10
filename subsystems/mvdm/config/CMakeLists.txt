
# NTVDM Default Configuration Files

list(APPEND NTVDM_CONFIG_FILES
     AUTOEXEC.NT
     CONFIG.NT)

foreach(item ${NTVDM_CONFIG_FILES})
    add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/${item}" DESTINATION VerOS/system32 FOR all)
    add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/${item}" DESTINATION VerOS/repair FOR all)
endforeach(item)

# system.ini
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/system.ini DESTINATION VerOS FOR all)

# win.ini
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/win.ini DESTINATION VerOS FOR all)
