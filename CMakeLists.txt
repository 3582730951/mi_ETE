cmake_minimum_required(VERSION 3.16)
project(mi_ete LANGUAGES C CXX)

option(BUILD_SERVER "Build server component" ON)
option(BUILD_CLIENT_WINDOWS "Build Windows client" ON)
option(BUILD_CLIENT_ANDROID "Enable Android client placeholder" OFF)
option(BUILD_SHARED_TESTS "Build shared/client tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(shared)

if(BUILD_SERVER)
  add_subdirectory(server)
endif()

if(BUILD_CLIENT_WINDOWS)
  add_subdirectory(client/windows)
endif()
