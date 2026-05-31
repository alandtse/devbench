# devbench-api — header-only cross-plugin API for SKSE consumers.
# Mirrors the SkyrimVRESL port: installs the MIT API header + its companion .cpp
# (compiled into the consumer via the config's INTERFACE_SOURCES).
#
# Pinned to a published commit. devbench-api isn't in the official vcpkg registry, so
# consumers add this directory to VCPKG_OVERLAY_PORTS (see README). To ship a newer API
# revision, bump REF to the new commit and SHA512 to its archive hash.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO alandtse/devbench
    REF 2f0dac74f409f895b8b74e5440be8371e33fe0b0
    SHA512 0bc46d6a09cf0b216373b1ec09d8da8b338b58b90618f3b1b8cea07616875a7fc85c6e777166a0998213f0e73e7ea099e20142591fe3de77f36113241985ae3e
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
