# =============================================================================
# Star Wars: Knights of the Old Republic - static recompilation host (32-bit, MSVC x86)
#
#   py -3 run_lift.py --all  # -> src/recomp/gen/  (needs work/functions.json)
#   build.cmd                # vcvarsall x86 + CMake + Ninja -> build/kotor.exe
#   build\kotor.exe --run --watchdog 60
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(kotor C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall x86)")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py --all` first")
endif()

add_executable(kotor
  src/runtime/host.c
  src/runtime/oracle.c
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(kotor PRIVATE
  src/runtime src/recomp/gen ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
# RECOMP_RETADDR=0u: a stack imbalance during bring-up leaks a harmless 0, not
# 0xDEAD0000 into a size argument (forcecommander).
target_compile_definitions(kotor PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                     RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so the guest's 0x00400000..0x00836000 is free.
target_link_options(kotor PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x1000000
                                /MANIFESTUAC:NO)
set(GEN_OPT "/O2" CACHE STRING "optimisation for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(src/runtime/host.c src/runtime/oracle.c PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(kotor PRIVATE user32 gdi32 kernel32 ole32 uuid)

option(KOTOR_TRACE "record a ring buffer of lifted function entries" OFF)
if(KOTOR_TRACE)
  target_compile_definitions(kotor PRIVATE RECOMP_TRACE=1)
endif()
