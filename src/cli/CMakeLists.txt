cmake_minimum_required(VERSION 3.18)

set(SOURCES main.cpp OptionParser.cpp OptionParser.h)

find_library(SYSTEMD_LIB systemd)
find_library(ASYNCNS_LIB asyncns)

add_executable(lgx2userspace ${SOURCES})
target_include_directories(lgx2userspace PRIVATE liblgx)
target_link_libraries(lgx2userspace PRIVATE liblgx)
if(SYSTEMD_LIB)
    target_link_libraries(lgx2userspace PRIVATE ${SYSTEMD_LIB})
endif()
if(ASYNCNS_LIB)
    target_link_libraries(lgx2userspace PRIVATE ${ASYNCNS_LIB})
endif()

# Viewing lgx2d's audio needs the PipeWire library (built when libpipewire-0.3 is found).
if (TARGET liblgx-pipewire)
    target_link_libraries(lgx2userspace PRIVATE liblgx-pipewire)
    target_compile_definitions(lgx2userspace PRIVATE LGX2_VIEWER_PIPEWIRE)
endif()
