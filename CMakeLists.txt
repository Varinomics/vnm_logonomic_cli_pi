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

add_library(pi_provider STATIC src/pi_launcher.cpp src/pi_preparation.cpp src/pi_lifecycle.cpp src/pi_sessions.cpp src/pi_profile.cpp)
pi_target(pi_provider)
target_include_directories(pi_provider PUBLIC src)
target_link_libraries(pi_provider PUBLIC logonomic_cli_addon_sdk::sdk)

add_executable(pi_adapter src/adapter.cpp)
target_link_libraries(pi_adapter PRIVATE pi_provider)
pi_target(pi_adapter)

add_executable(pi_helper src/pi_helper.cpp)
pi_target(pi_helper)

logonomic_cli_addon_add_launch_bridge(pi_launch_bridge)
pi_target(pi_launch_bridge)

logonomic_cli_addon_package_platform(PI_PLATFORM)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(pi_package logonomic.cli.pi PI_PACKAGE_DIRECTORY
    ADAPTER pi_adapter
    HELPERS pi_helper pi_launch_bridge)

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_executable(pi_contract_tests tests/pi_contract_tests.cpp)
pi_target(pi_contract_tests)
add_dependencies(pi_contract_tests pi_package)

add_executable(pi_session_tests tests/pi_session_tests.cpp)
target_link_libraries(pi_session_tests PRIVATE pi_provider)
pi_target(pi_session_tests)
add_dependencies(pi_session_tests pi_package)
add_test(NAME pi_native_sessions COMMAND pi_session_tests "${PI_PACKAGE_DIRECTORY}")
set_tests_properties(pi_native_sessions PROPERTIES TIMEOUT 120 LABELS "cli-addon;native-history")

add_executable(pi_profile_tests tests/pi_profile_tests.cpp)
target_link_libraries(pi_profile_tests PRIVATE pi_provider)
pi_target(pi_profile_tests)
add_dependencies(pi_profile_tests pi_package)
add_test(NAME pi_accounts_contract COMMAND pi_profile_tests "${PI_PACKAGE_DIRECTORY}")
set_tests_properties(pi_accounts_contract PROPERTIES TIMEOUT 120 LABELS "cli-addon;native-profile")
if(WIN32)
    add_test(NAME pi_case_sensitive_accounts COMMAND pi_profile_tests --case-profiles "${PI_PACKAGE_DIRECTORY}")
    set_tests_properties(pi_case_sensitive_accounts PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "cli-addon;native-profile")
endif()

find_program(PI_NODE_EXECUTABLE NAMES node node.exe)
set(PI_TEST_PI_CLI "" CACHE FILEPATH "Optional installed Pi CLI JavaScript entrypoint for native runtime qualification")
set(pi_contract_arguments "${PI_PACKAGE_DIRECTORY}")
if(PI_NODE_EXECUTABLE)
    list(APPEND pi_contract_arguments "${PI_NODE_EXECUTABLE}")
    if(PI_TEST_PI_CLI)
        list(APPEND pi_contract_arguments "${PI_TEST_PI_CLI}")
    endif()
endif()
if(PI_NODE_EXECUTABLE AND PI_TEST_PI_CLI)
    add_test(NAME pi_native_profiles
        COMMAND "${PI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pi_profile_smoke.mjs"
            "${PI_TEST_PI_CLI}" "$<TARGET_FILE:pi_profile_tests>")
    set_tests_properties(pi_native_profiles PROPERTIES TIMEOUT 60 LABELS "cli-addon;native-profile")
endif()
if(PI_NODE_EXECUTABLE AND UNIX)
    add_executable(pi_managed_launcher_tests tests/pi_managed_launcher_tests.cpp)
    target_link_libraries(pi_managed_launcher_tests PRIVATE pi_provider)
    pi_target(pi_managed_launcher_tests)
    add_test(NAME pi_managed_launchers
        COMMAND pi_managed_launcher_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pi-managed" "${PI_NODE_EXECUTABLE}")
    set_tests_properties(pi_managed_launchers PROPERTIES
        TIMEOUT 30 LABELS "cli-addon;provider-contract")
endif()
if(PI_NODE_EXECUTABLE)
    if(WIN32)
        add_executable(pi_fixture_script tests/pi_fixture_script_win.cpp)
        pi_target(pi_fixture_script)
        target_compile_definitions(pi_fixture_script PRIVATE PI_TEST_NODE="${PI_NODE_EXECUTABLE}")
        if(MINGW)
            target_link_options(pi_fixture_script PRIVATE -municode)
        endif()
    endif()
    add_test(NAME pi_extension_contract
        COMMAND "${PI_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/tests/pi_extension_tests.mjs")
    set_tests_properties(pi_extension_contract PROPERTIES TIMEOUT 120 LABELS "cli-addon;native-extension")
    if(PI_TEST_PI_CLI)
        add_test(NAME pi_native_runtime
            COMMAND "${PI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pi_runtime_smoke.mjs"
                "${PI_TEST_PI_CLI}" "$<TARGET_FILE:pi_helper>"
                "$<TARGET_FILE:logonomic_cli_addon_host_simulator>" "${PI_PACKAGE_DIRECTORY}")
        set_tests_properties(pi_native_runtime PROPERTIES TIMEOUT 300 LABELS "cli-addon;native-extension")
        add_test(NAME pi_native_resume
            COMMAND "${PI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pi_resume_smoke.mjs"
                "${PI_TEST_PI_CLI}" "${PI_PACKAGE_DIRECTORY}" "$<TARGET_FILE:pi_session_tests>")
        set_tests_properties(pi_native_resume PROPERTIES TIMEOUT 120 LABELS "cli-addon;native-history")
        add_test(NAME pi_native_profile_handoff
            COMMAND "${PI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pi_profile_handoff_smoke.mjs"
                "${PI_TEST_PI_CLI}" "${PI_PACKAGE_DIRECTORY}" "$<TARGET_FILE:pi_profile_tests>")
        set_tests_properties(pi_native_profile_handoff PROPERTIES TIMEOUT 120 LABELS "cli-addon;native-profile")
    endif()
    if(WIN32)
        foreach(test pi_extension_contract pi_native_runtime pi_native_resume pi_native_profile_handoff)
            if(TEST ${test})
                set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "PI_TEST_SCRIPT_RUNNER=$<TARGET_FILE:pi_fixture_script>")
            endif()
        endforeach()
    endif()
endif()
add_test(NAME pi_provider_contract COMMAND pi_contract_tests ${pi_contract_arguments})
add_test(NAME pi_telemetry_contract COMMAND pi_contract_tests "${PI_PACKAGE_DIRECTORY}" --telemetry)
add_test(NAME pi_package_validation
    COMMAND logonomic_cli_addon_validate "${PI_PACKAGE_DIRECTORY}" --platform "${PI_PLATFORM}")
add_test(NAME pi_package_conformance
    COMMAND logonomic_cli_addon_conformance "${PI_PACKAGE_DIRECTORY}")
set_tests_properties(pi_provider_contract pi_telemetry_contract pi_package_validation pi_package_conformance PROPERTIES
    TIMEOUT 120 LABELS "cli-addon;provider-contract")

if(WIN32 AND PI_NODE_EXECUTABLE)
    get_filename_component(pi_node_directory "${PI_NODE_EXECUTABLE}" DIRECTORY)
    find_file(PI_NPM_CLI npm-cli.js HINTS "${pi_node_directory}/node_modules/npm/bin")
    if(PI_NPM_CLI)
        add_test(NAME pi_npm_launchers
            COMMAND "${PI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pi_npm_launchers.mjs"
                "${PI_NPM_CLI}" "$<TARGET_FILE:logonomic_cli_addon_host_simulator>" "${PI_PACKAGE_DIRECTORY}")
        set_tests_properties(pi_npm_launchers PROPERTIES TIMEOUT 120 LABELS "cli-addon;provider-contract")
    endif()
endif()
