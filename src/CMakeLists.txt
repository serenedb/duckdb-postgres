include_directories(include)

add_subdirectory(dbconnector)
add_subdirectory(storage)

add_library(
  postgres_ext_library OBJECT
  postgres_attach.cpp
  postgres_aws.cpp
  postgres_binary_copy.cpp
  postgres_binary_file_reader.cpp
  postgres_binary_parser.cpp
  postgres_binary_reader.cpp
  postgres_connection.cpp
  postgres_copy_from.cpp
  postgres_copy_to.cpp
  postgres_execute.cpp
  postgres_extension.cpp
  postgres_filter_pushdown.cpp
  postgres_hstore.cpp
  postgres_parameters.cpp
  postgres_query.cpp
  postgres_scanner.cpp
  postgres_secrets.cpp
  postgres_storage.cpp
  postgres_text_reader.cpp
  postgres_utils.cpp
  postgres_logging.cpp
  postgres_oauth.cpp)

# Pull libpq's INTERFACE_INCLUDE_DIRECTORIES (libpq-fe.h, libpq-int.h, the
# pg_config.h shipped under cmake/libpq/) into the compile of these .cpp's.
# Using OBJECT libs means there's no link step here — this is the only way to
# get header propagation.
target_link_libraries(postgres_ext_library PRIVATE PostgreSQL::PostgreSQL)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:postgres_ext_library>
    PARENT_SCOPE)
