
add_rc_deps(rosbrand.rc
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/bmp/brand_banner.bmp
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/bmp/brand_bannerline.bmp)
add_library(rosbrand MODULE
    rosbrand.rc)

set_entrypoint(rosbrand 0)
add_cd_file(TARGET rosbrand DESTINATION VerOS/system32 FOR all)
