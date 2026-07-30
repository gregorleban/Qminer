cmake_minimum_required(VERSION 3.16)
project(qminer_cpp_tests CXX)

# ---------------------------------------------------------------------------
# GoogleTest via FetchContent (replaces the dead googlecode gtest-1.7.0 download).
# Pinned version; downloaded + built from source, so no system/apt gtest needed.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
)
FetchContent_MakeAvailable(googletest)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# qminer / glib source + prebuilt static libs.
#   GLIB_DIR / QMINER_DIR : header include roots
#   QMINER_LIB_DIR        : where glib.a and qminer.a live (build/Release by
#                           default; override with -DQMINER_LIB_DIR=... )
# glib.a is produced by `make -C src/glib`; qminer.a by the node-gyp build.
# ---------------------------------------------------------------------------
set(QMINER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(GLIB_DIR   "${QMINER_ROOT}/src/glib")
set(QMINER_DIR "${QMINER_ROOT}/src/qminer")
set(SOLE_DIR   "${QMINER_ROOT}/src/third_party/sole")
set(QMINER_LIB_DIR "${QMINER_ROOT}/build/Release" CACHE PATH "dir containing glib.a and qminer.a")

find_library(GLIB_A   NAMES glib   glib.a   PATHS "${QMINER_LIB_DIR}" "${GLIB_DIR}" NO_DEFAULT_PATH)
find_library(QMINER_A NAMES qminer qminer.a PATHS "${QMINER_LIB_DIR}" NO_DEFAULT_PATH)

include_directories(
  "${GLIB_DIR}/base" "${GLIB_DIR}/mine" "${GLIB_DIR}/net" "${GLIB_DIR}/misc"
  "${GLIB_DIR}/concurrent" "${QMINER_DIR}" "${SOLE_DIR}"
)

# Compiler flags mirroring test/cpp/Makefile (Linux)
add_compile_options(-fopenmp -w)
add_definitions(-DNDEBUG)

# ---------------------------------------------------------------------------
# Test sources. run-all-tests.cpp provides main() (gtest). Every other *.cpp
# in this dir is a test translation unit — glob so newly added tests are picked
# up automatically (the old Makefile hard-listed only 4 files).
# ---------------------------------------------------------------------------
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER TEST_SOURCES EXCLUDE REGEX "/run-all-tests\\.cpp$")
list(FILTER TEST_SOURCES EXCLUDE REGEX "/run_tests\\.")   # js runner, not a TU
list(FILTER TEST_SOURCES EXCLUDE REGEX "/test_main\\.cpp$") # vestigial old main; main comes from run-all-tests.cpp

# (The stale hyphen test-*.cpp duplicates of the maintained test_*.cpp files,
# and the broken test-aggr.cpp, were deleted 2026-07-30 - no exclusions needed.)

# glib.a's mine.o wraps LIBSVM but the implementation is a separate third-party
# TU (built by binding.gyp on the node path) - compile it into the test binary
set(LIBSVM_SRC "${QMINER_ROOT}/src/third_party/libsvm/svm.cpp")
include_directories("${QMINER_ROOT}/src/third_party/libsvm")

add_executable(qminer_tests ${TEST_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/run-all-tests.cpp" "${LIBSVM_SRC}")
target_link_libraries(qminer_tests
  PRIVATE gtest gtest_main
  "${QMINER_A}" "${GLIB_A}"
  gomp pthread rt)

# Optional system libs: needed only if the linker pulls glib.a members that use
# them (sockets->uv, guid->uuid, regex->re2). Present on CI (apt); often absent
# on a dev WSL without sudo - the tests don't touch those members, so skip them.
foreach(optlib uuid uv re2)
  find_library(OPTLIB_${optlib} ${optlib})
  if(OPTLIB_${optlib})
    target_link_libraries(qminer_tests PRIVATE ${OPTLIB_${optlib}})
  endif()
endforeach()

# copy fixture dirs next to the binary so tests that read them find them
foreach(fixdir data files)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${fixdir}")
    add_custom_command(TARGET qminer_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/${fixdir}" "$<TARGET_FILE_DIR:qminer_tests>/${fixdir}")
  endif()
endforeach()

# The Storage tests write their bases into ./test/cpp/data (relative to the repository
# root, where the tests run). That directory holds only generated data and is therefore
# gitignored, so it is absent from a fresh checkout - and the tests then quietly caught
# the resulting "cannot open file" exceptions and reported success without testing
# anything. Create it up front.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data")

enable_testing()

# Register the whole binary as ONE ctest test, deliberately not gtest_discover_tests:
# the suite is not process-isolated. Storage.TInit performs one-time global set-up
# (TUnicodeDef::Load) that the later Storage and PgBlob tests rely on, and those tests
# also share fixture state through ./test/cpp/data. Running each test in its own
# process - what gtest_discover_tests does - makes them abort. The tests also reference
# fixtures by paths relative to the repository root, hence WORKING_DIRECTORY.
add_test(NAME qminer_tests
  COMMAND qminer_tests --gtest_output=xml:${CMAKE_BINARY_DIR}/test-results.xml
  WORKING_DIRECTORY "${QMINER_ROOT}")
