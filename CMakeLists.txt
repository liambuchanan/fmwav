cmake_minimum_required(VERSION 3.28)
set(CMAKE_CXX_STANDARD 17)
project(fmwav)

find_package(PkgConfig)
pkg_check_modules(PC_RTLSDR REQUIRED librtlsdr)
find_path(RTLSDR_INCLUDE_DIR NAMES rtl-sdr.h)
find_library(RTLSDR_LIBRARY NAMES rtlsdr)

add_executable(fmwav fmwav.c)
target_include_directories(fmwav PRIVATE ${RTLSDR_INCLUDE_DIR})
target_link_libraries(fmwav PRIVATE ${RTLSDR_LIBRARY})
