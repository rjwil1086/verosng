# Hook for the boot CD after milestone 13. Do not include this file from
# the OS CMake build while that upgrade work owns the boot image.

function(verosupd_cargo target)
    set(_manifest "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml")
    set(_triple "x86_64-pc-windows-gnu")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/cargo")
    add_custom_target(${target} ALL
        COMMAND clang -O2 -D_CRT_SECURE_NO_WARNINGS -o "${CMAKE_CURRENT_LIST_DIR}/clang-link.exe" "${CMAKE_CURRENT_LIST_DIR}/clang-link.c"
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${CMAKE_CURRENT_LIST_DIR};${VEROSNG_ROOT}/tools/clang/bin;${VEROSNG_ROOT}/tools/gcc/bin;${VEROSNG_ROOT}/tools/rust/cargo/bin;$ENV{PATH}"
            "RUSTUP_HOME=${VEROSNG_ROOT}/tools/rust/rustup"
            "CARGO_HOME=${VEROSNG_ROOT}/tools/rust/cargo"
            cargo build --manifest-path "${_manifest}" --target ${_triple} --release --target-dir "${_out}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        COMMENT "Building VerOS update client"
        VERBATIM)
endfunction()
