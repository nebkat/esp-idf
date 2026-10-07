# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# The project version as `git describe` gives it, for PROJECT_VER_AT_BUILD.
#
# Included by project.cmake, which describes the version once at configure time. Run with
# 'cmake -D REPO_DIR=<dir> -D HEADER=<file> -P' as the app builds, it writes HEADER again,
# so that a new commit changes the app's version without reconfiguring the project.

# __project_ver_describe(<var> <repo_dir>)
#
# Unlike git_describe(), this does not make the repository's HEAD a configure input.
# Sets <var> empty when the version cannot be described.
function(__project_ver_describe var repo_dir)
    if(NOT GIT_FOUND)
        find_package(Git QUIET)
    endif()
    set(${var} "" PARENT_SCOPE)
    if(NOT GIT_FOUND)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${repo_dir}" describe --always --tags --dirty
        RESULT_VARIABLE result
        OUTPUT_VARIABLE version
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(result EQUAL 0)
        set(${var} "${version}" PARENT_SCOPE)
    endif()
endfunction()

# __project_ver_write_header(<header> <version>)
#
# The header esp_app_desc.c includes (the APP_DESC_HEADER build property), cut to the 31
# characters the app description holds. It is written only when its contents change, so
# that an unchanged version rebuilds nothing.
function(__project_ver_write_header header version)
    string(SUBSTRING "${version}" 0 31 version)
    file(CONFIGURE OUTPUT "${header}" CONTENT "#define PROJECT_VER \"@version@\"\n" @ONLY)
endfunction()

set(__PROJECT_VER_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

# __project_ver_at_build(<repo_dir> <build_dir> <version>)
#
# Has the app take its version from a header that a build step describes again every
# build, starting from <version> as described at configure time.
function(__project_ver_at_build repo_dir build_dir version)
    set(header "${build_dir}/project_ver.h")
    __project_ver_write_header("${header}" "${version}")
    idf_build_set_property(APP_DESC_HEADER "${header}")
    add_custom_target(project_ver ALL
        COMMAND "${CMAKE_COMMAND}" -D "REPO_DIR=${repo_dir}" -D "HEADER=${header}" -P "${__PROJECT_VER_SCRIPT}"
        BYPRODUCTS "${header}"
        VERBATIM)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    __project_ver_describe(version "${REPO_DIR}")
    if(version)
        __project_ver_write_header("${HEADER}" "${version}")
    endif()
endif()
