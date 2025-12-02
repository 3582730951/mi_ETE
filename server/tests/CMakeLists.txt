add_executable(mi_server_auth_tests
    auth_service_tests.cpp
)

add_executable(mi_server_media_tests
    media_integration_tests.cpp
)

add_executable(mi_server_data_tests
    data_route_tests.cpp
)

add_executable(mi_server_config_tests
    config_tests.cpp
)

target_link_libraries(mi_server_auth_tests
    PRIVATE
    mi_server_core
    mi_shared
)

target_link_libraries(mi_server_media_tests
    PRIVATE
    mi_server_core
    mi_shared
)

target_link_libraries(mi_server_data_tests
    PRIVATE
    mi_server_core
    mi_shared
)

target_link_libraries(mi_server_config_tests
    PRIVATE
    mi_server_core
    mi_shared
)

if(MSVC)
  target_compile_options(mi_server_auth_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_server_media_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_server_data_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_server_config_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(mi_server_auth_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_server_media_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_server_data_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_server_config_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mi_server_auth
    COMMAND mi_server_auth_tests
)

add_test(
    NAME mi_server_media
    COMMAND mi_server_media_tests
)

add_test(
    NAME mi_server_data
    COMMAND mi_server_data_tests
)

add_test(
    NAME mi_server_config
    COMMAND mi_server_config_tests
)
