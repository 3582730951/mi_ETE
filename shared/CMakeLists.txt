set(SHARED_SOURCES
    third_party/ikcp.c
    src/kcp_channel.cpp
    src/tcp_tunnel.cpp
    src/whitebox_aes.cpp
    src/messages.cpp
    src/disordered_file.cpp
    src/chat_history.cpp
    src/placeholder.cpp
    src/cert_store.cpp
    src/tls_support.cpp
)

add_library(mi_shared STATIC ${SHARED_SOURCES})
target_include_directories(mi_shared
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Windows/MSVC 使用 CNG，避免链接未知 OpenSSL 安装；其他平台若存在则启用
if(NOT MSVC)
  find_package(OpenSSL QUIET)
  if(OPENSSL_FOUND)
    target_include_directories(mi_shared PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(mi_shared PUBLIC MI_HAS_OPENSSL)
    target_link_libraries(mi_shared PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  endif()
endif()

target_compile_definitions(mi_shared
    PUBLIC
    MI_SHARED_UNICODE
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
)

if(WIN32)
  target_link_libraries(mi_shared
      PRIVATE
      ws2_32
      crypt32
      ncrypt
      bcrypt
  )
endif()

if(BUILD_SHARED_TESTS)
  add_subdirectory(tests)
endif()
