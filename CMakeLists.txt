cmake_minimum_required(VERSION 3.14)

project(iqdb)

# Require C++17
# https://cmake.org/cmake/help/latest/prop_tgt/CXX_STANDARD.html
# https://cmake.org/cmake/help/latest/prop_tgt/CXX_STANDARD_REQUIRED.html
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Disable GNU-specific C++-17 features.
# https://cmake.org/cmake/help/latest/prop_tgt/CXX_EXTENSIONS.html#prop_tgt:CXX_EXTENSIONS
# https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Extensions.html
set(CMAKE_CXX_EXTENSIONS OFF)

# https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_COMPILE_COMMANDS.html
# https://clang.llvm.org/docs/JSONCompilationDatabase.html#supported-systems
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# https://stackoverflow.com/questions/1620918/cmake-and-libpthread
set(THREADS_PREFER_PTHREAD_FLAG ON)

# https://cmake.org/cmake/help/latest/policy/CMP0135.html
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
  GIT_TAG        v0.15.3
)

FetchContent_Declare(
  json
  URL            https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.6.0
)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        10.2.1
)

FetchContent_Declare(
  sqliteOrm
  GIT_REPOSITORY https://github.com/fnc12/sqlite_orm
  GIT_TAG        v1.8.2
)

FetchContent_MakeAvailable(httplib)
FetchContent_MakeAvailable(json)
FetchContent_MakeAvailable(Catch2)
FetchContent_MakeAvailable(fmt)
FetchContent_MakeAvailable(sqliteOrm)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

add_subdirectory(src)
