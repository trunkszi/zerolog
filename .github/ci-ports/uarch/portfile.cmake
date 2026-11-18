vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

if (NOT VCPKG_TARGET_IS_LINUX)
    message(FATAL_ERROR "uarch is only wired for Linux in this overlay; no other target has a reflection-capable toolchain here.")
endif ()

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/trunkszi/uarch.git
    REF 66e88f220c5bbf372ff901e6b528a0247d8a51ef
    FETCH_REF main
    HEAD_REF main
)

# A port must resolve dependencies from its installed tree, not a nested manifest.
file(REMOVE "${SOURCE_PATH}/vcpkg.json" "${SOURCE_PATH}/vcpkg-configuration.json")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DVCPKG_MANIFEST_MODE=OFF
        -DCMAKE_INSTALL_LIBDIR=lib
        -DUARCH_AUTO_DOWNLOAD_TOOLCHAIN=OFF
        -DUARCH_BUILD_TESTS=OFF
        -DUARCH_BUILD_EXAMPLES=OFF
        -DUARCH_BUILD_BENCHES=OFF
        -DUARCH_BUILD_TOOLS=OFF
        -DUARCH_ENABLE_SANITIZERS=OFF
        -DUARCH_INSTALL_PACKAGE=ON
    MAYBE_UNUSED_VARIABLES
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_RELEASE
        CMAKE_INSTALL_LIBDIR
        VCPKG_MANIFEST_MODE
)

vcpkg_cmake_install()

# These configuration-independent helpers are installed once beside the merged targets.
file(REMOVE
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake/uarch/UarchToolchain.cmake"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake/uarch/UarchReflection.cmake"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake/uarch/UarchCompilerOptions.cmake"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake/uarch/UarchRuntime.cmake"
)
vcpkg_cmake_config_fixup(PACKAGE_NAME uarch CONFIG_PATH lib/cmake/uarch)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
