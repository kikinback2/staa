cmake_minimum_required(VERSION 3.10)
project(dfvr_bridge)

find_package(Protobuf REQUIRED)

include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ../proto/dfvr_bridge.proto)

add_library(dfvr_bridge SHARED src/dfvr_bridge.cpp ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(dfvr_bridge ${Protobuf_LIBRARIES})

# Standalone Mock Executable for testing without DFHack
add_executable(dfvr_bridge_mock src/dfvr_bridge.cpp ${PROTO_SRCS} ${PROTO_HDRS})
target_compile_definitions(dfvr_bridge_mock PRIVATE STANDALONE_MOCK)
target_link_libraries(dfvr_bridge_mock ${Protobuf_LIBRARIES})
