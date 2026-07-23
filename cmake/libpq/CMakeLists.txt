# Build description for libpq, the client library piece of postgres.
#
# Adapted from ClickHouse's contrib/postgres-cmake/CMakeLists.txt
# (https://github.com/ClickHouse/ClickHouse/tree/master/contrib/postgres-cmake)
# with paths rewritten for our duckdb-postgres tree.
#
# Replaces the previous flow that ran postgres ./configure + make at build
# time. The three pre-generated `pg_config{,_os,_paths}.h` headers in this
# directory stand in for what configure would have produced — generic enough
# for Linux x86_64 + macOS, which is all SereneDB targets.

set(POSTGRES_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../postgres")
set(LIBPQ_SOURCE_DIR "${POSTGRES_SOURCE_DIR}/src/interfaces/libpq")

set(SRCS
    "${LIBPQ_SOURCE_DIR}/fe-auth.c"
    "${LIBPQ_SOURCE_DIR}/fe-auth-scram.c"
    "${LIBPQ_SOURCE_DIR}/fe-auth-oauth.c"
    "${LIBPQ_SOURCE_DIR}/fe-cancel.c"
    "${LIBPQ_SOURCE_DIR}/fe-connect.c"
    "${LIBPQ_SOURCE_DIR}/fe-exec.c"
    "${LIBPQ_SOURCE_DIR}/fe-lobj.c"
    "${LIBPQ_SOURCE_DIR}/fe-misc.c"
    "${LIBPQ_SOURCE_DIR}/fe-print.c"
    "${LIBPQ_SOURCE_DIR}/fe-trace.c"
    "${LIBPQ_SOURCE_DIR}/fe-protocol3.c"
    "${LIBPQ_SOURCE_DIR}/fe-secure.c"
    "${LIBPQ_SOURCE_DIR}/fe-secure-common.c"
    "${LIBPQ_SOURCE_DIR}/fe-secure-openssl.c"
    "${LIBPQ_SOURCE_DIR}/legacy-pqsignal.c"
    "${LIBPQ_SOURCE_DIR}/libpq-events.c"
    "${LIBPQ_SOURCE_DIR}/pqexpbuffer.c"

    "${POSTGRES_SOURCE_DIR}/src/common/scram-common.c"
    "${POSTGRES_SOURCE_DIR}/src/common/sha2.c"
    "${POSTGRES_SOURCE_DIR}/src/common/sha1.c"
    "${POSTGRES_SOURCE_DIR}/src/common/md5.c"
    "${POSTGRES_SOURCE_DIR}/src/common/md5_common.c"
    "${POSTGRES_SOURCE_DIR}/src/common/hmac_openssl.c"
    "${POSTGRES_SOURCE_DIR}/src/common/cryptohash.c"
    "${POSTGRES_SOURCE_DIR}/src/common/saslprep.c"
    "${POSTGRES_SOURCE_DIR}/src/common/unicode_norm.c"
    "${POSTGRES_SOURCE_DIR}/src/common/ip.c"
    "${POSTGRES_SOURCE_DIR}/src/common/jsonapi.c"
    "${POSTGRES_SOURCE_DIR}/src/common/wchar.c"
    "${POSTGRES_SOURCE_DIR}/src/common/base64.c"
    "${POSTGRES_SOURCE_DIR}/src/common/link-canary.c"
    "${POSTGRES_SOURCE_DIR}/src/common/fe_memutils.c"
    "${POSTGRES_SOURCE_DIR}/src/common/string.c"
    "${POSTGRES_SOURCE_DIR}/src/common/pg_get_line.c"
    "${POSTGRES_SOURCE_DIR}/src/common/pg_prng.c"
    "${POSTGRES_SOURCE_DIR}/src/common/stringinfo.c"
    "${POSTGRES_SOURCE_DIR}/src/common/psprintf.c"
    "${POSTGRES_SOURCE_DIR}/src/common/encnames.c"
    "${POSTGRES_SOURCE_DIR}/src/common/logging.c"

    "${POSTGRES_SOURCE_DIR}/src/port/snprintf.c"
    "${POSTGRES_SOURCE_DIR}/src/port/strerror.c"
    "${POSTGRES_SOURCE_DIR}/src/port/inet_net_ntop.c"
    "${POSTGRES_SOURCE_DIR}/src/port/getpeereid.c"
    "${POSTGRES_SOURCE_DIR}/src/port/chklocale.c"
    "${POSTGRES_SOURCE_DIR}/src/port/noblock.c"
    "${POSTGRES_SOURCE_DIR}/src/port/pg_strong_random.c"
    "${POSTGRES_SOURCE_DIR}/src/port/pgstrcasecmp.c"
    "${POSTGRES_SOURCE_DIR}/src/port/pg_bitutils.c"
    "${POSTGRES_SOURCE_DIR}/src/port/path.c"
    "${POSTGRES_SOURCE_DIR}/src/port/timingsafe_bcmp.c"
)

if(NOT OS_DARWIN)
    list(APPEND SRCS
        "${POSTGRES_SOURCE_DIR}/src/port/strlcat.c"
        "${POSTGRES_SOURCE_DIR}/src/port/strlcpy.c"
    )
endif()

if(OS_DARWIN)
    list(APPEND SRCS
        "${POSTGRES_SOURCE_DIR}/src/port/explicit_bzero.c"
    )
endif()

add_library(libpq STATIC ${SRCS})

target_compile_definitions(libpq PRIVATE
    FRONTEND
    _GNU_SOURCE
    OPENSSL_NO_ENGINE
)

target_include_directories(libpq SYSTEM PUBLIC
    # Our pg_config*.h must shadow any stale autotools-generated ones that an
    # older configure run may have left in postgres/src/include — list our
    # cmake dir first.
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LIBPQ_SOURCE_DIR}"
    "${POSTGRES_SOURCE_DIR}/src/include"
)

# Some BSD-flavoured systems return int from strerror_r (XSI signature) instead
# of char*; postgres' port code branches on STRERROR_R_INT for that.
if(OS_DARWIN OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(libpq PRIVATE STRERROR_R_INT=1)
endif()

if(OS_DARWIN)
    target_compile_definitions(libpq PRIVATE OS_DARWIN)
endif()

target_link_libraries(libpq PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# Match the standard FindPostgreSQL imported target so the extension's
# top-level CMakeLists doesn't need to know we vendor libpq.
add_library(PostgreSQL::PostgreSQL ALIAS libpq)
