# Generated protobuf + gRPC bindings, consumed by both server and client.

set(MINIMAL_PROTO_FILES
    minimal/v1/common.proto
    minimal/v1/session_service.proto)

add_library(minimal_proto STATIC ${MINIMAL_PROTO_FILES})
add_library(minimal::proto ALIAS minimal_proto)

target_link_libraries(minimal_proto
    PUBLIC protobuf::libprotobuf gRPC::grpc++)

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
