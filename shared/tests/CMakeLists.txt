add_executable(mi_shared_crypto_tests
    whitebox_aes_tests.cpp
)

add_executable(mi_shared_messages_tests
    messages_tests.cpp
)

add_executable(mi_shared_session_list_tests
    session_list_tests.cpp
)

add_executable(mi_shared_kcp_tests
    kcp_channel_tests.cpp
)

add_executable(mi_shared_storage_tests
    disordered_file_tests.cpp
)

add_executable(mi_shared_chat_history_tests
    chat_history_tests.cpp
)

add_executable(mi_shared_tcp_tunnel_tests
    tcp_tunnel_tests.cpp
)

add_executable(mi_shared_secure_tests
    obfuscated_value_tests.cpp
)

target_link_libraries(mi_shared_crypto_tests
    PRIVATE
    mi_shared
)

target_link_libraries(mi_shared_messages_tests
    PRIVATE
    mi_shared
)

target_link_libraries(mi_shared_session_list_tests
    PRIVATE
    mi_shared
)

target_link_libraries(mi_shared_kcp_tests
    PRIVATE
    mi_shared
)

target_link_libraries(mi_shared_storage_tests
    PRIVATE
    mi_shared
)

target_link_libraries(mi_shared_chat_history_tests
    PRIVATE
    mi_shared
)

target_link_libraries(mi_shared_tcp_tunnel_tests
    PRIVATE
    mi_shared
)

target_link_libraries(mi_shared_secure_tests
    PRIVATE
    mi_shared
)

if(MSVC)
  target_compile_options(mi_shared_crypto_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_shared_messages_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_shared_session_list_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_shared_kcp_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_shared_storage_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_shared_chat_history_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_shared_tcp_tunnel_tests PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(mi_shared_secure_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(mi_shared_crypto_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_shared_messages_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_shared_session_list_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_shared_kcp_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_shared_storage_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_shared_chat_history_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_shared_tcp_tunnel_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(mi_shared_secure_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mi_shared_crypto
    COMMAND mi_shared_crypto_tests
)

add_test(
    NAME mi_shared_messages
    COMMAND mi_shared_messages_tests
)

add_test(
    NAME mi_shared_session_list
    COMMAND mi_shared_session_list_tests
)

add_test(
    NAME mi_shared_kcp
    COMMAND mi_shared_kcp_tests
)

add_test(
    NAME mi_shared_storage
    COMMAND mi_shared_storage_tests
)

add_test(
    NAME mi_shared_chat_history
    COMMAND mi_shared_chat_history_tests
)

add_test(
    NAME mi_shared_tcp_tunnel
    COMMAND mi_shared_tcp_tunnel_tests
)

add_test(
    NAME mi_shared_secure
    COMMAND mi_shared_secure_tests
)
