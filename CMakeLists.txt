cmake_minimum_required(VERSION 3.25)

project(dbox LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "dbox builds with clang >= 18 or gcc >= 14, not ${CMAKE_C_COMPILER_ID}")
endif()

option(DBOX_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# What `dbox version` prints: the nearest tag, else the commit, else "dev".
set(DBOX_VERSION "" CACHE STRING "Version string baked into the binary")
if(DBOX_VERSION STREQUAL "")
    execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE DBOX_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(DBOX_VERSION STREQUAL "")
        set(DBOX_VERSION "dev")
    endif()
endif()

find_package(CURL REQUIRED)

# ---- vendored dependencies, built with their own flags ----

add_library(sqlite STATIC deps/sqlite/sqlite3.c)
target_include_directories(sqlite PUBLIC deps/sqlite)
target_compile_definitions(sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
target_compile_options(sqlite PRIVATE -w)
set_target_properties(sqlite PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

file(GLOB LIBYAML_SOURCES CONFIGURE_DEPENDS deps/libyaml/src/*.c)
add_library(libyaml STATIC ${LIBYAML_SOURCES})
target_include_directories(libyaml PUBLIC deps/libyaml/include PRIVATE deps/libyaml/src)
target_compile_definitions(libyaml
    PUBLIC YAML_DECLARE_STATIC
    PRIVATE YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5")
target_compile_options(libyaml PRIVATE -w)
set_target_properties(libyaml PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

# ---- dbox ----

set(DBOX_WARNINGS -Wall -Wextra -Werror -Wconversion -Wshadow -Wvla -Wstrict-prototypes -Wimplicit-fallthrough -Wno-unused-parameter)

file(GLOB DBOX_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM DBOX_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)
list(APPEND DBOX_SOURCES src/platform/posix.c)
if(APPLE)
    list(APPEND DBOX_SOURCES src/platform/macos.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DBOX_SOURCES src/platform/linux.c)
else()
    message(FATAL_ERROR "dbox targets macOS and Linux; ${CMAKE_SYSTEM_NAME} has no platform implementation")
endif()

add_library(dbox_core STATIC ${DBOX_SOURCES})
target_include_directories(dbox_core PUBLIC src)
target_compile_options(dbox_core PUBLIC ${DBOX_WARNINGS})
target_compile_definitions(dbox_core PUBLIC _DEFAULT_SOURCE _GNU_SOURCE _DARWIN_C_SOURCE)
target_link_libraries(dbox_core PUBLIC sqlite libyaml CURL::libcurl m)
find_package(Threads REQUIRED)
target_link_libraries(dbox_core PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(dbox_core PUBLIC "-framework CoreServices")
endif()

if(DBOX_SANITIZE)
    set(SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(dbox_core PUBLIC ${SANITIZE_FLAGS})
    target_link_options(dbox_core PUBLIC ${SANITIZE_FLAGS})
endif()

add_executable(dbox src/main.c)
target_link_libraries(dbox PRIVATE dbox_core)
target_compile_definitions(dbox PRIVATE DBOX_VERSION="${DBOX_VERSION}")

# ---- tests ----

enable_testing()

add_executable(test_dbox tests/test_dbox.c)
target_link_libraries(test_dbox PRIVATE dbox_core)
add_test(NAME unit COMMAND test_dbox)

# The S3 store against the two MinIOs from docker-compose.yml; `tools/minio.sh` starts them.
add_executable(test_s3 tests/test_s3.c)
target_link_libraries(test_s3 PRIVATE dbox_core)
add_test(NAME integration COMMAND test_s3)
set_tests_properties(integration PROPERTIES LABELS integration)

# ---- check: static analysis over the project's own sources ----

find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    add_custom_target(check
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} --warnings-as-errors=* ${DBOX_SOURCES} ${CMAKE_SOURCE_DIR}/src/main.c
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "clang-tidy over src/"
        VERBATIM)
else()
    add_custom_target(check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found; install it to run the check target" && exit 1
        VERBATIM)
endif()

# ---- install and uninstall ----

install(TARGETS dbox RUNTIME DESTINATION bin)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -DDBOX_BINARY=${CMAKE_INSTALL_PREFIX}/bin/dbox -P ${CMAKE_SOURCE_DIR}/cmake/uninstall.cmake
    COMMENT "Disabling the login service and removing ${CMAKE_INSTALL_PREFIX}/bin/dbox"
    VERBATIM)
