# paperdiy-reader: e-reader firmware for the M5Stack PaperDIY (ESP32-S3).
#
# Components (M5GFX / M5Unified with PaperDIY support) are pinned in
# repos.json and fetched by `python fetch_repos.py`; the font pack is built by
# `python tools/make_fonts.py`. See README.md.
cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
# Re-run CMake when the version changes, so the new number is baked in.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/version.txt")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(paperdiy_reader)
