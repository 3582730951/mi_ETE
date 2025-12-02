add_executable(mi_client_secure_types_tests
    secure_types_tests.cpp
)

target_link_libraries(mi_client_secure_types_tests
    PRIVATE
    mi_client_core
    mi_shared
)

if(MSVC)
  target_compile_options(mi_client_secure_types_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(mi_client_secure_types_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mi_client_secure_types
    COMMAND mi_client_secure_types_tests
)
