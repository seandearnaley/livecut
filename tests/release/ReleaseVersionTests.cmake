# SPDX-License-Identifier: GPL-3.0-or-later
# Runs without configuring or building Livecut.
cmake_minimum_required(VERSION 3.25)
set(fixture "${CMAKE_CURRENT_BINARY_DIR}/release-version-fixture")
file(MAKE_DIRECTORY "${fixture}/packaging")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../../packaging/check-release-version.cmake"
               "${fixture}/packaging/check-release-version.cmake" COPYONLY)
file(WRITE "${fixture}/CMakeLists.txt" "project(Livecut VERSION 0.3.0 LANGUAGES C CXX)\n")

function(check_tag tag expected_success)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DRELEASE_TAG=${tag}" -P "${fixture}/packaging/check-release-version.cmake"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "Expected ${tag} to pass: ${output}${error}")
    elseif(NOT expected_success AND result EQUAL 0)
        message(FATAL_ERROR "Expected ${tag} to fail")
    endif()
endfunction()

check_tag("v0.3.0" TRUE)
check_tag("v0.3.0-rc.1" TRUE)
check_tag("v0.4.0" FALSE)
check_tag("0.3.0" FALSE)
check_tag("v0.3" FALSE)
check_tag("v0.3.0-junk/invalid" FALSE)
check_tag("" FALSE)
file(WRITE "${fixture}/CMakeLists.txt" "project(AnotherProject VERSION 0.3.0 LANGUAGES C CXX)\n")
check_tag("v0.3.0" FALSE)
file(REMOVE_RECURSE "${fixture}")
message(STATUS "Release version checks passed")
