cmake_minimum_required(VERSION 3.10)
project(dfvr_bridge)

find_package(Protobuf REQUIRED)

include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Add DFHack includes (Paths should be adjusted based on the DFHack build tree)
# include_directories(${DFHACK_INCLUDES})

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ../../proto/dfvr_bridge.proto)

add_library(dfvr_bridge SHARED src/dfvr_bridge.cpp ${PROTO_SRCS} ${PROTO_HDRS})

target_link_libraries(dfvr_bridge ${Protobuf_LIBRARIES})
# target_link_libraries(dfvr_bridge dfhack-core)
