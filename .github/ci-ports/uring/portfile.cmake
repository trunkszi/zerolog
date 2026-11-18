vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/trunkszi/uring.git
    REF 2b2109ef2e01df7e90199ce9f49330312d4cb1d0
    FETCH_REF main
    HEAD_REF main
)

file(REMOVE "${SOURCE_PATH}/vcpkg.json" "${SOURCE_PATH}/vcpkg-configuration.json")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DVCPKG_MANIFEST_MODE=OFF
        -DCMAKE_INSTALL_LIBDIR=lib
        -DURING_AUTO_DOWNLOAD_TOOLCHAIN=OFF
        -DURING_BUILD_TESTS=OFF
        -DURING_BUILD_TOOLS=OFF
        -DURING_BUILD_EXAMPLES=OFF
        -DURING_BUILD_BENCHES=OFF
        -DURING_ENABLE_SANITIZERS=OFF
        -DURING_INSTALL_PACKAGE=ON
    MAYBE_UNUSED_VARIABLES
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_RELEASE
        CMAKE_INSTALL_LIBDIR
        VCPKG_MANIFEST_MODE
)

vcpkg_cmake_install()

file(REMOVE
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake/uring/UringToolchain.cmake"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake/uring/UringReflection.cmake"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake/uring/UringCompilerOptions.cmake"
)
vcpkg_cmake_config_fixup(PACKAGE_NAME uring CONFIG_PATH lib/cmake/uring)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
