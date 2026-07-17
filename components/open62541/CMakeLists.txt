# open62541 generates its public amalgamation from the pinned upstream source.
# Generation is isolated in the ESP-IDF build directory, so the source tree is
# never modified and an incremental build can reuse the generated files.
set(OPEN62541_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/vendor")
set(OPEN62541_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(OPEN62541_SOURCE "${OPEN62541_GENERATED_DIR}/open62541.c")
set(OPEN62541_HEADER "${OPEN62541_GENERATED_DIR}/open62541.h")

# ESP-IDF executes component CMakeLists.txt once in CMake script mode to collect
# dependencies. ExternalProject is not scriptable, so the early pass must stop
# after publishing this component's requirements.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(
        INCLUDE_DIRS "${OPEN62541_GENERATED_DIR}"
        REQUIRES lwip vfs sock_utils
    )
    return()
endif()

include(ExternalProject)
file(MAKE_DIRECTORY "${OPEN62541_GENERATED_DIR}")

ExternalProject_Add(open62541_codegen
    SOURCE_DIR "${OPEN62541_VENDOR_DIR}"
    BINARY_DIR "${OPEN62541_GENERATED_DIR}"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_BUILD_TYPE=MinSizeRel"
        "-DOPEN62541_VERSION=v1.4.14"
        "-DUA_ARCHITECTURE=posix"
        "-DUA_ENABLE_AMALGAMATION=ON"
        "-DUA_NAMESPACE_ZERO=REDUCED"
        "-DUA_ENABLE_SUBSCRIPTIONS=ON"
        "-DUA_ENABLE_SUBSCRIPTIONS_EVENTS=ON"
        "-DUA_ENABLE_DISCOVERY=OFF"
        "-DUA_ENABLE_DISCOVERY_MULTICAST=OFF"
        "-DUA_ENABLE_ENCRYPTION=OFF"
        "-DUA_ENABLE_PUBSUB=OFF"
        "-DUA_ENABLE_PUBSUB_INFORMATIONMODEL=OFF"
        "-DUA_ENABLE_PUBSUB_ENCRYPTION=OFF"
        "-DUA_ENABLE_PUBSUB_SKS=OFF"
        "-DUA_ENABLE_PUBSUB_FILE_CONFIG=OFF"
        "-DUA_ENABLE_PUBSUB_MONITORING=OFF"
        "-DUA_ENABLE_PUBSUB_BUFMALLOC=OFF"
        "-DUA_ENABLE_HISTORIZING=OFF"
        "-DUA_ENABLE_JSON_ENCODING=OFF"
        "-DUA_ENABLE_XML_ENCODING=OFF"
        "-DUA_ENABLE_STATUSCODE_DESCRIPTIONS=ON"
        "-DUA_ENABLE_TYPEDESCRIPTION=ON"
        "-DUA_LOGLEVEL=300"
        "-DUA_BUILD_EXAMPLES=OFF"
        "-DUA_BUILD_UNIT_TESTS=OFF"
        "-DUA_BUILD_TOOLS=OFF"
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build "${OPEN62541_GENERATED_DIR}"
        --target open62541-code-generation
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS "${OPEN62541_SOURCE}" "${OPEN62541_HEADER}"
)

# Rebuild the amalgamation when an ESP portability source changes. Otherwise
# ExternalProject's completed stamp can leave a stale generated open62541.c.
ExternalProject_Add_StepDependencies(open62541_codegen build
    "${OPEN62541_VENDOR_DIR}/arch/posix/ua_clock.c"
)

set_source_files_properties("${OPEN62541_SOURCE}" PROPERTIES GENERATED TRUE)

idf_component_register(
    SRCS "${OPEN62541_SOURCE}"
    INCLUDE_DIRS "${OPEN62541_GENERATED_DIR}"
    REQUIRES lwip vfs sock_utils
)

add_dependencies(${COMPONENT_LIB} open62541_codegen)

# The amalgamated header uses this definition to expose the single-file API.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    UA_ENABLE_AMALGAMATION
    _DEFAULT_SOURCE
    _POSIX_C_SOURCE=200809L
)

# Keep warnings strict for project code without treating upstream diagnostics as
# firmware errors. This option is private to the vendored translation unit.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
