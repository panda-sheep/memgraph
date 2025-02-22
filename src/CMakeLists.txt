# CMake configuration for the main memgraph library and executable

# add memgraph sub libraries, ordered by dependency
add_subdirectory(lisp)
add_subdirectory(utils)
add_subdirectory(requests)
add_subdirectory(io)
add_subdirectory(kvstore)
add_subdirectory(telemetry)
add_subdirectory(communication)
add_subdirectory(memory)
add_subdirectory(storage/v2)
add_subdirectory(integrations)
add_subdirectory(query)
add_subdirectory(slk)
add_subdirectory(rpc)
add_subdirectory(auth)

if (MG_ENTERPRISE)
  add_subdirectory(audit)
endif()

string(TOLOWER ${CMAKE_BUILD_TYPE} lower_build_type)

# Generate a version.hpp file
set(VERSION_STRING ${MEMGRAPH_VERSION})
configure_file(version.hpp.in version.hpp @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# ----------------------------------------------------------------------------
# Memgraph Single Node v2 Executable
# ----------------------------------------------------------------------------
set(mg_single_node_v2_sources
    glue/communication.cpp
    memgraph.cpp
    glue/auth.cpp
)

set(mg_single_node_v2_libs stdc++fs Threads::Threads
    telemetry_lib mg-query mg-communication mg-memory mg-utils mg-auth mg-license mg-settings)
if (MG_ENTERPRISE)
  # These are enterprise subsystems
  set(mg_single_node_v2_libs ${mg_single_node_v2_libs} mg-audit)
endif()

# memgraph main executable
add_executable(memgraph ${mg_single_node_v2_sources})
target_include_directories(memgraph PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(memgraph ${mg_single_node_v2_libs})
# NOTE: `include/mg_procedure.syms` describes a pattern match for symbols which
# should be dynamically exported, so that `dlopen` can correctly link the
# symbols in custom procedure module libraries.
target_link_libraries(memgraph "-Wl,--dynamic-list=${CMAKE_SOURCE_DIR}/include/mg_procedure.syms")
set_target_properties(memgraph PROPERTIES
                      # Set the executable output name to include version information.
                      OUTPUT_NAME "memgraph-${MEMGRAPH_VERSION}_${CMAKE_BUILD_TYPE}"
                      # Output the executable in main binary dir.
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# Create symlink to the built executable.
add_custom_command(TARGET memgraph POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE:memgraph> ${CMAKE_BINARY_DIR}/memgraph
                   BYPRODUCTS ${CMAKE_BINARY_DIR}/memgraph
                   COMMENT "Creating symlink to memgraph executable")
# Emulate the installed python_support, by creating a symlink
add_custom_command(TARGET memgraph POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/python_support
                   BYPRODUCTS ${CMAKE_BINARY_DIR}/python_support
                   COMMENT "Creating symlink for python_support")

# Strip the executable in release build.
if (lower_build_type STREQUAL "release")
  add_custom_command(TARGET memgraph POST_BUILD
                     COMMAND strip -s $<TARGET_FILE:memgraph>
                     COMMENT "Stripping symbols and sections from memgraph")
endif()

# Generate the configuration file.
add_custom_command(TARGET memgraph POST_BUILD
                   COMMAND ${CMAKE_SOURCE_DIR}/config/generate.py
                               ${CMAKE_BINARY_DIR}/memgraph
                               ${CMAKE_BINARY_DIR}/config/memgraph.conf
                   DEPENDS ${CMAKE_SOURCE_DIR}/config/generate.py
                           ${CMAKE_SOURCE_DIR}/config/flags.yaml
                   BYPRODUCTS ${CMAKE_BINARY_DIR}/config/memgraph.conf
                   COMMENT "Generating memgraph configuration file")

# Everything here is under "memgraph" install component.
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "memgraph")

# TODO: Default directory permissions to 755
# NOTE: This is added in CMake 3.11, so enable it then
#set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
#    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)

# Install and rename executable to just 'memgraph' Since we have to rename,
# we cannot use the recommended `install(TARGETS ...)`.
install(PROGRAMS $<TARGET_FILE:memgraph>
        DESTINATION lib/memgraph RENAME memgraph)
# Install Python source for supporting our embedded Python.
install(FILES ${CMAKE_SOURCE_DIR}/include/mgp.py
        DESTINATION lib/memgraph/python_support)
# Install the include file for writing custom procedures.
install(FILES ${CMAKE_SOURCE_DIR}/include/mg_procedure.h
        DESTINATION include/memgraph)
# Install the config file (must use absolute path).
install(FILES ${CMAKE_BINARY_DIR}/config/memgraph.conf
        DESTINATION /etc/memgraph RENAME memgraph.conf)
# Install logrotate configuration (must use absolute path).
install(FILES ${CMAKE_SOURCE_DIR}/release/logrotate.conf
        DESTINATION /etc/logrotate.d RENAME memgraph)

# Create empty directories for default location of lib and log.
install(CODE "file(MAKE_DIRECTORY \$ENV{DESTDIR}/var/log/memgraph
                   \$ENV{DESTDIR}/var/lib/memgraph)")

# ----------------------------------------------------------------------------
# END Memgraph Single Node v2 Executable
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# Memgraph CSV Import Tool Executable
# ----------------------------------------------------------------------------

add_executable(mg_import_csv mg_import_csv.cpp)
target_link_libraries(mg_import_csv mg-storage-v2)

# Strip the executable in release build.
if (lower_build_type STREQUAL "release")
  add_custom_command(TARGET mg_import_csv POST_BUILD
                     COMMAND strip -s mg_import_csv
                     COMMENT "Stripping symbols and sections from mg_import_csv")
endif()

install(TARGETS mg_import_csv RUNTIME DESTINATION bin)

# ----------------------------------------------------------------------------
# Memgraph CSV Import Tool Executable
# ----------------------------------------------------------------------------
