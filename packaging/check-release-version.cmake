# SPDX-License-Identifier: GPL-3.0-or-later
# The installer/archive tag and the embedded plug-in version must agree.
if(NOT DEFINED RELEASE_TAG OR NOT RELEASE_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z][0-9A-Za-z.-]*)?$")
    message(FATAL_ERROR "RELEASE_TAG must be vMAJOR.MINOR.PATCH, optionally followed by a prerelease suffix")
endif()
set(tag_version "${CMAKE_MATCH_1}")

file(READ "${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt" project_source)
if(NOT project_source MATCHES "project\\(Livecut[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)[ \t\r\n]")
    message(FATAL_ERROR "Cannot find Livecut's embedded version in CMakeLists.txt")
endif()
set(project_version "${CMAKE_MATCH_1}")

if(NOT tag_version STREQUAL project_version)
    message(FATAL_ERROR "Release tag ${RELEASE_TAG} does not match embedded Livecut version ${project_version}")
endif()
message(STATUS "Release tag ${RELEASE_TAG} matches embedded Livecut version ${project_version}")
