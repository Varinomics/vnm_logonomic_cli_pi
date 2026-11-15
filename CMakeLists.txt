cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_pi VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must add the Logonomic CLI add-on SDK first.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(pi_logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(pi_logonomic_sdk_source)
    endif()
endif()

function(pi_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(pi_provider STATIC src/pi_launcher.cpp)
pi_target(pi_provider)
target_include_directories(pi_provider PUBLIC src)
target_link_libraries(pi_provider PUBLIC logonomic_cli_addon_sdk::sdk)

add_executable(pi_adapter src/adapter.cpp)
target_link_libraries(pi_adapter PRIVATE pi_provider)
pi_target(pi_adapter)

logonomic_cli_addon_package_platform(PI_PLATFORM)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(pi_package logonomic.cli.pi PI_PACKAGE_DIRECTORY
    ADAPTER pi_adapter)

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_test(NAME pi_package_validation
    COMMAND logonomic_cli_addon_validate "${PI_PACKAGE_DIRECTORY}" --platform "${PI_PLATFORM}")
add_test(NAME pi_package_conformance
    COMMAND logonomic_cli_addon_conformance "${PI_PACKAGE_DIRECTORY}")
set_tests_properties(pi_package_validation pi_package_conformance PROPERTIES
    TIMEOUT 120 LABELS "cli-addon;provider-contract")
