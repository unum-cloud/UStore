include(ExternalProject)

set(OPENSSL_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/openssl-src)
set(OPENSSL_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/openssl)
set(OPENSSL_LIB_DIR ${OPENSSL_INSTALL_DIR}/lib)
set(OPENSSL_INCLUDE_DIR ${OPENSSL_INSTALL_DIR}/include)

set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_INSTALL_DIR}/lib)
set(OPENSSL_SSL_LIBRARY ${OPENSSL_INSTALL_DIR}/lib)
set(OPENSSL_CONFIGURE_COMMAND ${OPENSSL_SOURCE_DIR}/config)
set(openssl_DIR ${OPENSSL_INSTALL_DIR})

ExternalProject_Add(
  openssl-external
  SOURCE_DIR ${OPENSSL_SOURCE_DIR}
  GIT_REPOSITORY https://github.com/openssl/openssl.git
  GIT_TAG openssl-3.1.0
  CONFIGURE_COMMAND ${OPENSSL_CONFIGURE_COMMAND}
  --prefix=${OPENSSL_INSTALL_DIR}
  --openssldir=${OPENSSL_INSTALL_DIR}
  --libdir=${OPENSSL_LIB_DIR}

  BUILD_COMMAND make
  TEST_COMMAND ""
  INSTALL_DIR ${OPENSSL_INSTALL_DIR}
  INSTALL_COMMAND make install

  BUILD_ALWAYS 0
  UPDATE_DISCONNECTED 1
)
file(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})

add_library(openssl::ssl STATIC IMPORTED)
set_property(TARGET openssl::ssl PROPERTY IMPORTED_LOCATION ${OPENSSL_LIB_DIR}/libssl.a)
set_property(TARGET openssl::ssl PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
add_dependencies(openssl::ssl openssl-external)

add_library(openssl::crypto STATIC IMPORTED GLOBAL)
set_property(TARGET openssl::crypto PROPERTY IMPORTED_LOCATION ${OPENSSL_LIB_DIR}/libcrypto.a)
set_property(TARGET openssl::crypto PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
add_dependencies(openssl::crypto openssl-external)