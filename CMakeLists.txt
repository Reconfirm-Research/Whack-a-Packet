cmake_minimum_required(VERSION 3.18.4)
project(whack C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -pedantic")
endif()

# Define _GNU_SOURCE
add_definitions(-D_GNU_SOURCE)

# Find required packages
find_package(PkgConfig REQUIRED)

# Function to find BPF library and headers
function(find_bpf)
    find_path(LIBBPF_INCLUDE_DIR
        NAMES
            bpf/bpf.h
            bpf/btf.h
            bpf/libbpf.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/local/include
            ENV CPATH)

    find_library(LIBBPF_LIBRARIES
        NAMES
            bpf
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/local/lib
            ENV LIBRARY_PATH
            ENV LD_LIBRARY_PATH)

    if(NOT LIBBPF_INCLUDE_DIR OR NOT LIBBPF_LIBRARIES)
        message(FATAL_ERROR "libbpf not found. Please install libbpf-dev")
    endif()

    set(LIBBPF_INCLUDE_DIR ${LIBBPF_INCLUDE_DIR} PARENT_SCOPE)
    set(LIBBPF_LIBRARIES ${LIBBPF_LIBRARIES} PARENT_SCOPE)
endfunction()

# Find BPF
find_bpf()

# Find XDP
pkg_check_modules(LIBXDP REQUIRED libxdp)
if(NOT LIBXDP_FOUND)
    message(FATAL_ERROR "libxdp not found. Please install libxdp-dev")
endif()

# Find NUMA
pkg_check_modules(NUMA REQUIRED numa)
if(NOT NUMA_FOUND)
    message(FATAL_ERROR "libnuma not found. Please install libnuma-dev")
endif()

# Find required headers
find_path(KERNEL_HEADERS
    NAMES linux/if_xdp.h linux/if_link.h
    PATHS /usr/include /usr/local/include
    REQUIRED
)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${KERNEL_HEADERS}
    ${LIBBPF_INCLUDE_DIR}
    ${LIBXDP_INCLUDE_DIRS}
    ${NUMA_INCLUDE_DIRS}
)

# Source files
set(SOURCES
    src/main.c
    src/af_xdp_init.c
    src/dns_query.c
    src/cache.c
)

# Create executable
add_executable(whack ${SOURCES})

# Link directories
link_directories(
    ${LIBXDP_LIBRARY_DIRS}
    ${NUMA_LIBRARY_DIRS}
)

# Link libraries
target_link_libraries(whack
    ${LIBBPF_LIBRARIES}
    ${LIBXDP_LIBRARIES}
    ${NUMA_LIBRARIES}
    pthread
    elf
    z
)

# Installation
install(TARGETS whack
    RUNTIME DESTINATION bin
)

# Add custom target for format checking
find_program(CLANG_FORMAT "clang-format")
if(CLANG_FORMAT)
    add_custom_target(
        format
        COMMAND ${CLANG_FORMAT}
        -i
        ${SOURCES}
        ${CMAKE_SOURCE_DIR}/include/*.h
    )
endif()

# Generate compile_commands.json for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Print configuration summary
message(STATUS "Configuration Summary")
message(STATUS "-------------------")
message(STATUS "Kernel Headers: ${KERNEL_HEADERS}")
message(STATUS "libbpf Include: ${LIBBPF_INCLUDE_DIR}")
message(STATUS "libbpf Libraries: ${LIBBPF_LIBRARIES}")
message(STATUS "libxdp Include: ${LIBXDP_INCLUDE_DIRS}")
message(STATUS "libnuma Include: ${NUMA_INCLUDE_DIRS}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Flags: ${CMAKE_C_FLAGS}")

# Verify all required components are found
if(NOT LIBBPF_LIBRARIES OR NOT LIBXDP_FOUND OR NOT NUMA_FOUND OR NOT KERNEL_HEADERS)
    message(FATAL_ERROR "Missing required dependencies. Please check the error messages above.")
endif()
