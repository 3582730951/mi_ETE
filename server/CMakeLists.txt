add_library(mi_server_core
    src/config.cpp
    src/server_app.cpp
    src/auth_service.cpp
    src/panel_service.cpp
    src/message_router.cpp
)

target_include_directories(mi_server_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mi_server_core
    PRIVATE
    mi_shared
)

add_executable(mi_server
    src/main.cpp
)

target_link_libraries(mi_server
    PRIVATE
    mi_server_core
    mi_shared
)

if(MSVC)
  target_compile_options(mi_server_core PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_server PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(mi_server_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_SHARED_TESTS)
  add_subdirectory(tests)
endif()
