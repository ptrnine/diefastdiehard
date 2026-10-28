# Drop-in replacement for CMake's FindOpenAL.cmake, used when cross-compiling
# for Windows/MinGW.
#
# The system module searches for the library under the names
# OpenAL/al/openal/OpenAL32. CMake only does case-insensitive library-name
# matching when it can detect a case-insensitive filesystem, which is not
# true when cross-compiling from Linux — so "OpenAL32" never matches SFML's
# bundled extlibs/libs-mingw/x64/libopenal32.a.
#
# This file is copied to SFML's cmake/Modules/ (which SFML's
# sfml_find_package puts at the front of CMAKE_MODULE_PATH) by
# build-windows.sh. It lives in the repo root under CMake/patches/ so the
# submodule stays a clean checkout.
find_library(OPENAL_LIBRARY NAMES openal32)
find_path(OPENAL_INCLUDE_DIR al.h)

if(OPENAL_LIBRARY AND OPENAL_INCLUDE_DIR)
    set(OPENAL_FOUND TRUE)
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenAL
    REQUIRED_VARS OPENAL_LIBRARY OPENAL_INCLUDE_DIR
    VERSION_VAR OPENAL_VERSION_STRING)
mark_as_advanced(OPENAL_LIBRARY OPENAL_INCLUDE_DIR)

if(OPENAL_FOUND AND NOT TARGET OpenAL::OpenAL)
    add_library(OpenAL::OpenAL UNKNOWN IMPORTED)
    set_target_properties(OpenAL::OpenAL PROPERTIES
        IMPORTED_LOCATION "${OPENAL_LIBRARY}")
    set(OpenAL_INCLUDE_DIRS "${OPENAL_INCLUDE_DIR}")
endif()
