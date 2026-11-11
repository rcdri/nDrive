cmake_minimum_required(VERSION 3.24)
project(ndrive LANGUAGES C CXX ASM)

find_package(Ndless 0.1 CONFIG REQUIRED)

set(NDRIVE_SOURCES
    src/main.c src/usbhook.c src/usbhook_gate.S src/usbbg.c src/mtp.c src/msc.c src/vfat.c
    src/calcfs.c src/notify.c src/resident.c src/resident_gate.S
    third_party/fatfs/ff.c third_party/fatfs/ffunicode.c)
file(GLOB NDRIVE_HEADERS ${CMAKE_SOURCE_DIR}/src/*.h)

set(BUILD_ID_H ${CMAKE_BINARY_DIR}/gen/build_id.h)
list(TRANSFORM NDRIVE_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/ OUTPUT_VARIABLE NDRIVE_ABS)
add_custom_command(
    OUTPUT ${BUILD_ID_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/build_id.py ${BUILD_ID_H}
            "${CMAKE_C_COMPILER_VERSION} ${CMAKE_BUILD_TYPE}" ${NDRIVE_ABS} ${NDRIVE_HEADERS}
            ${CMAKE_SOURCE_DIR}/CMakeLists.txt
    DEPENDS ${NDRIVE_ABS} ${NDRIVE_HEADERS} ${CMAKE_SOURCE_DIR}/tools/build_id.py ${CMAKE_SOURCE_DIR}/CMakeLists.txt
    VERBATIM)

add_nspire_program(ndrive
    SOURCES ${NDRIVE_SOURCES} ${BUILD_ID_H}
    NAME "nDrive"
    GENZEHN_FLAGS --240x320-support true --uses-lcd-blit true
    GC_SECTIONS)
target_include_directories(ndrive PRIVATE src third_party/fatfs ${CMAKE_BINARY_DIR}/gen)
target_compile_options(ndrive PRIVATE -marm -Os -Wall -Wextra -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99>)
