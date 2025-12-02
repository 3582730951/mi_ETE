add_library(mi_client_core
    src/secure_types.cpp
    src/client_runner.cpp
)

option(BUILD_CLIENT_QT "Build Qt GUI client (requires Qt5/Qt6 Widgets)" ON)

target_include_directories(mi_client_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mi_client_core
    PRIVATE
    mi_shared
)
target_compile_definitions(mi_client_core
    PUBLIC
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
)

add_executable(mi_client
    src/main.cpp
)

target_link_libraries(mi_client
    PRIVATE
    mi_client_core
    mi_shared
)

if(MSVC)
  target_compile_options(mi_client_core PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_client PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(mi_client_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_SHARED_TESTS)
  add_subdirectory(tests)
endif()

if(BUILD_CLIENT_QT)
  set(QT_MIN_VERSION 5.15)
  find_package(Qt6 COMPONENTS Widgets Network Multimedia QUIET)
  if(NOT Qt6_FOUND)
    find_package(Qt5 COMPONENTS Widgets Network Multimedia QUIET)
  endif()

  if(Qt6_FOUND OR Qt5_FOUND)
    set(QT_LIBS Qt6::Widgets Qt6::Network)
    if(Qt5_FOUND)
      set(QT_LIBS Qt5::Widgets Qt5::Network)
    endif()
    if(Qt6Multimedia_FOUND)
      list(APPEND QT_LIBS Qt6::Multimedia)
    endif()
    if(Qt5Multimedia_FOUND)
      list(APPEND QT_LIBS Qt5::Multimedia)
    endif()

    set(QT_SOURCES
        src/qt_main.cpp
        src/qt_window.cpp
        src/qt_window.hpp
    )
    add_library(mi_client_qt_ui SHARED ${QT_SOURCES})
    set_target_properties(mi_client_qt_ui PROPERTIES AUTOMOC ON)
    target_link_libraries(mi_client_qt_ui PRIVATE ${QT_LIBS} mi_client_core mi_shared)
    if(Qt6Multimedia_FOUND OR Qt5Multimedia_FOUND)
      target_compile_definitions(mi_client_qt_ui PRIVATE MI_ENABLE_QTMULTIMEDIA=1)
    endif()
    if(MSVC)
      target_compile_options(mi_client_qt_ui PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(mi_client_qt_ui PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  else()
    message(WARNING "BUILD_CLIENT_QT=ON 但未找到 Qt5/Qt6 Widgets，跳过 mi_client_qt 目标，仅构建 CLI 版本")
  endif()
endif()
