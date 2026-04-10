# don't export any symbols from the third party stuff
set(CMAKE_C_VISIBILITY_PRESET hidden)

# Download and include CPR HTTP client library
include(FetchContent)
FetchContent_Declare(
  cpr
  GIT_REPOSITORY https://github.com/whoshuu/cpr.git
  GIT_TAG        1.12.0  # prepend 'v' to match the actual tag name
)
# Use system-installed libcurl rather than embedded build
set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "Use system libcurl" FORCE)
FetchContent_MakeAvailable(cpr)
# Make CPR headers available globally
include_directories(${cpr_SOURCE_DIR}/include)

if(NOT AMALGAMATION_BUILD)
  add_subdirectory(fmt)
  add_subdirectory(libpg_query)
  add_subdirectory(re2)
  add_subdirectory(miniz)
  add_subdirectory(utf8proc)
  add_subdirectory(hyperloglog)
  add_subdirectory(skiplist)
  add_subdirectory(fastpforlib)
  add_subdirectory(mbedtls)
  add_subdirectory(fsst)
  add_subdirectory(yyjson)
  add_subdirectory(zstd)
  # cpr is provided via FetchContent, no local subdir
endif()

if(NOT WIN32
   AND NOT SUN
   AND ${BUILD_UNITTESTS})
  add_subdirectory(imdb)
  if(${BUILD_TPCE})
    add_subdirectory(tpce-tool)
  endif()
endif()
