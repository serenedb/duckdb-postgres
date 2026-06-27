cmake_minimum_required(VERSION 2.8.12...3.29)
set(TARGET_NAME postgres_scanner)
project(${TARGET_NAME})

find_package(OpenSSL REQUIRED)

if(NOT MSVC)
    add_compile_options(
        -Wno-pedantic
        -Wno-sign-compare
        -Wno-unused-variable)
endif()

# Build libpq from the in-tree postgres submodule via a pure-CMake recipe
# (cmake/libpq/), adapted from ClickHouse's contrib/postgres-cmake. Replaces
# the previous flow that shelled out to postgres' autotools configure + make
# at build time. Defines a `libpq` static lib + a `PostgreSQL::PostgreSQL`
# alias to match what find_package(PostgreSQL) would have exposed.
add_subdirectory(cmake/libpq)

include_directories(
    include
    ../database-connector/src/include
    ${OPENSSL_INCLUDE_DIR})

add_subdirectory(src)

set(PARAMETERS "-no-warnings")
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

build_static_extension(${TARGET_NAME} ${ALL_OBJECT_FILES})
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${ALL_OBJECT_FILES})

# Fix for ELF symbol conflict when loaded inside PostgreSQL backend.
# Use -Bsymbolic to force the extension to prefer its own symbols over global
# ones — fixes the pg_link_canary_is_frontend() check failure where calls from
# within this extension resolve to the backend's version (returns false)
# instead of ours (returns true).
if(NOT WIN32 AND NOT APPLE)
    target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        "-Wl,-Bsymbolic"
    )
endif()

target_link_libraries(${LOADABLE_EXTENSION_NAME}
    PostgreSQL::PostgreSQL
    OpenSSL::SSL
    OpenSSL::Crypto
    ${CURL_LIBRARIES})
target_link_libraries(${EXTENSION_NAME}
    PostgreSQL::PostgreSQL
    OpenSSL::SSL
    OpenSSL::Crypto
    ${CURL_LIBRARIES})

if(WIN32)
  target_link_libraries(${LOADABLE_EXTENSION_NAME}
      wsock32
      ws2_32
      wldap32
      secur32
      crypt32)
endif()

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
