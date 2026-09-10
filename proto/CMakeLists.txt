# Generated protobuf + gRPC bindings, consumed by both server and client.

set(MINIMAL_PROTO_FILES
    minimal/v1/common.proto
    minimal/v1/session_service.proto)

# SHARED, not STATIC, and that matters now that the client core is shared too
# (E5b): a static archive linked into two shared libraries bakes TWO copies of
# protobuf/gRPC/abseil into any process that loads both cores -- which is every
# test binary, and the deadlock there was real (grpc_init blocking on another
# copy's absl mutex). One shared library, one copy in memory, one init.
add_library(minimal_proto SHARED ${MINIMAL_PROTO_FILES})
add_library(minimal::proto ALIAS minimal_proto)

# gRPC::grpc++ alone: it publicly links protobuf::libprotobuf itself, so also
# naming it here would put libprotobuf.a on the link line twice (ld notes the
# duplicate). Includes still propagate through grpc++'s interface.
target_link_libraries(minimal_proto
    PUBLIC gRPC::grpc++)

set_target_properties(minimal_proto PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")

# The cores (and anything else linking minimal::proto) reference it as
# @rpath/libminimal_proto.0.dylib; once installed, that resolves against THIS
# dylib's own LC_RPATH, not the loading executable's (macOS rpaths are not
# inherited). Proto sits in lib/ next to them, so its own directory is the
# answer; proto's other deps (grpc/protobuf) are static, nothing further needed.
if(APPLE)
    set_property(TARGET minimal_proto PROPERTY INSTALL_RPATH "@loader_path")
else()
    set_property(TARGET minimal_proto PROPERTY INSTALL_RPATH "$ORIGIN")
endif()

# Generated headers are included as "minimal/v1/....pb.h", so the generation root
# must be on the include path for anything that links this target.
target_include_directories(minimal_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

protobuf_generate(
    TARGET minimal_proto
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

protobuf_generate(
    TARGET minimal_proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# The cores are installed shared libraries (E5b), so this must be installed
# beside them -- without it, the installed binaries abort with
# "Library not loaded: @rpath/libminimal_proto.0.dylib".
install(TARGETS minimal_proto
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
