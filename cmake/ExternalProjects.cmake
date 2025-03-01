include(FindGit)
find_package(Git)
include(FindOpenSSL)
find_package(OpenSSL)
include (ExternalProject)
include (FetchContent)

include_directories(${CMAKE_INSTALL_PREFIX}/include)

# Find conan-generated package descriptions
list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_BINARY_DIR})
list(PREPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_BINARY_DIR})

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/conan.cmake")
  message(STATUS "Downloading conan.cmake from https://github.com/conan-io/cmake-conan")
  file(DOWNLOAD "https://raw.githubusercontent.com/conan-io/cmake-conan/0.18.1/conan.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/conan.cmake"
                TLS_VERIFY ON)
endif()

include(${CMAKE_CURRENT_BINARY_DIR}/conan.cmake)

conan_check(VERSION 1.53.0 REQUIRED)

# Enable revisions in the conan config
execute_process(COMMAND ${CONAN_CMD} config set general.revisions_enabled=1
                RESULT_VARIABLE RET_CODE)
if(NOT "${RET_CODE}" STREQUAL "0")
    message(FATAL_ERROR "Error setting revisions for Conan: '${RET_CODE}'")
endif()

conan_cmake_configure(
    REQUIRES
        "catch2/2.13.9@#8793d3e6287d3684201418de556d98fe"
        # These two dependencies are only needed to perform remote attestation
        # of SGX enclaves using Microsoft Azure's Attestation Service
        "jwt-cpp/0.6.0@#cd6b5c1318b29f4becaf807b23f7bb44"
        "picojson/cci.20210117@#2af3ad146959275c97a6957b87b9073f"
        # 26/04/2023 - Temporarily add RapidJSON as a CMake dependency, as
        # it was removed from faabric. Eventually consolidate to just using one
        # JSON (de-)serialising library
        "rapidjson/cci.20211112@#65b4e5feb6f1edfc8cbac0f669acaf17"
    GENERATORS
        cmake_find_package
        cmake_paths
)

conan_cmake_autodetect(FAABRIC_CONAN_SETTINGS)

conan_cmake_install(PATH_OR_REFERENCE .
                    BUILD outdated
                    UPDATE
                    REMOTE conancenter
                    PROFILE_HOST ${CMAKE_CURRENT_LIST_DIR}/../faabric/conan-profile.txt
                    PROFILE_BUILD ${CMAKE_CURRENT_LIST_DIR}/../faabric/conan-profile.txt
                    SETTINGS ${FAABRIC_CONAN_SETTINGS}
)

include(${CMAKE_CURRENT_BINARY_DIR}/conan_paths.cmake)

find_package(Catch2 REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(picojson REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(cpprestsdk REQUIRED)

# 22/12/2021 - WARNING: we don't install AWS through Conan as the recipe proved
# very unstable and failed frequently.

# There are some AWS docs on using the cpp sdk as an external project:
# https://github.com/aws/aws-sdk-cpp/blob/main/Docs/CMake_External_Project.md
# but they don't specify how to link the libraries, which required adding an
# extra couple of CMake targets.
set(AWS_CORE_LIBRARY ${CMAKE_INSTALL_PREFIX}/lib/libaws-cpp-sdk-core.so)
set(AWS_S3_LIBRARY ${CMAKE_INSTALL_PREFIX}/lib/libaws-cpp-sdk-s3.so)
ExternalProject_Add(aws_ext
    GIT_REPOSITORY   "https://github.com/aws/aws-sdk-cpp.git"
    GIT_TAG          "4bb8ef9677af4376c07eaff5ba5b9f8bc738d314" #1.10.53
    BUILD_ALWAYS     0
    TEST_COMMAND     ""
    UPDATE_COMMAND   ""
    BUILD_BYPRODUCTS ${AWS_S3_LIBRARY} ${AWS_CORE_LIBRARY}
    CMAKE_CACHE_ARGS "-DCMAKE_INSTALL_PREFIX:STRING=${CMAKE_INSTALL_PREFIX}"
    LIST_SEPARATOR    "|"
    CMAKE_ARGS       -DBUILD_SHARED_LIBS=ON
                     -DBUILD_ONLY=s3|sts
                     -DAUTORUN_UNIT_TESTS=OFF
                     -DENABLE_TESTING=OFF
                     -DCMAKE_BUILD_TYPE=Release
    LOG_CONFIGURE ON
    LOG_INSTALL ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON
)

add_library(aws_ext_core SHARED IMPORTED)
add_library(aws_ext_s3 SHARED IMPORTED)
set_target_properties(aws_ext_core
    PROPERTIES IMPORTED_LOCATION
    ${AWS_CORE_LIBRARY})
set_target_properties(aws_ext_s3
    PROPERTIES IMPORTED_LOCATION
    ${AWS_S3_LIBRARY})
add_dependencies(aws_ext_core aws_ext)
add_dependencies(aws_ext_s3 aws_ext)
# Merge the two libraries in one aliased interface
add_library(aws_ext_s3_lib INTERFACE)
target_link_libraries(aws_ext_s3_lib INTERFACE aws_ext_s3 aws_ext_core)
add_library(AWS::s3 ALIAS aws_ext_s3_lib)

# Tightly-coupled dependencies
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(wavm_ext
    GIT_REPOSITORY "https://github.com/ardhipoetra/WAVM.git"
    GIT_TAG "242d39da7c3e5df0b0a47dc1753e37e37cd60790"
    CMAKE_ARGS "-DDLL_EXPORT= \
        -DDLL_IMPORT="
)

set(wamr_patch git apply ${CMAKE_CURRENT_SOURCE_DIR}/wamr.patch)

FetchContent_Declare(wamr_ext
    GIT_REPOSITORY "https://github.com/faasm/wasm-micro-runtime"
    GIT_TAG "5e9dc3c7eb33167389d99b7e5851dc55b5911d33"
    PATCH_COMMAND ${wamr_patch}
    UPDATE_DISCONNECTED 1
)

# WAMR and WAVM both link to LLVM
# If WAVM is not linked statically like WAMR, there are some obscure
# static constructor errors in LLVM due to double-registration
set(WAVM_ENABLE_STATIC_LINKING ON CACHE INTERNAL "")

FetchContent_MakeAvailable(wavm_ext wamr_ext)

# Allow access to headers nested in other projects
FetchContent_GetProperties(wavm_ext SOURCE_DIR FAASM_WAVM_SOURCE_DIR)
message(STATUS FAASM_WAVM_SOURCE_DIR ${FAASM_WAVM_SOURCE_DIR})

FetchContent_GetProperties(wamr_ext SOURCE_DIR WAMR_ROOT_DIR)
message(STATUS WAMR_ROOT_DIR ${WAMR_ROOT_DIR})
