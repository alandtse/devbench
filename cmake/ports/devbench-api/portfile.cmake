# devbench-api — header-only cross-plugin API for SKSE consumers.
# Mirrors the SkyrimVRESL port: installs the MIT API header + its companion .cpp
# (compiled into the consumer via the config's INTERFACE_SOURCES).
#
# TODO(first-push): fill REF + SHA512 once devbench is published to GitHub. Until
# then, consume locally via an overlay port — point vcpkg at this directory:
#   vcpkg install devbench-api --overlay-ports=<devbench>/cmake/ports
# (with REF set to a local commit / using vcpkg_from_git on a file:// path), or add
# `"overlay-ports"` in the consumer's vcpkg-configuration.json. See README in this dir.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO alandtse/devbench
    REF 0000000000000000000000000000000000000000
    SHA512 0
    HEAD_REF main
)

# MIT API glue → header to include/, source to share/ (referenced by the config target).
file(INSTALL "${SOURCE_PATH}/include/DevBenchAPI.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${SOURCE_PATH}/include/DevBenchAPI.cpp"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/src")

# CMake package config — defines DevBench::API.
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/devbench-api-config.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

# The API glue is MIT (not the GPL-3.0 plugin) — ship that as the port copyright.
file(INSTALL "${SOURCE_PATH}/include/DevBenchAPI.LICENSE.txt"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
