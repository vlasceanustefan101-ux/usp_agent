cmake_minimum_required(VERSION 3.16)
project(tr369_controller VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(USP_WITH_TLS "Build STOMP client with TLS (OpenSSL) support" ON)
option(USP_BUILD_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)
# Newer protobuf releases ship a CMake config package; older ones use FindProtobuf.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()

# ---- Generated protobuf code (BBF USP 1.4 schemas + controller API) ----
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(PROTO_NAMES usp-record-1-4 usp-msg-1-4 usp-controller-api)
if(TARGET protobuf::protoc)
  set(PROTOC_EXE $<TARGET_FILE:protobuf::protoc>)
else()
  set(PROTOC_EXE ${Protobuf_PROTOC_EXECUTABLE})
endif()
file(MAKE_DIRECTORY ${GEN_DIR})
set(PROTO_SRCS)
foreach(name ${PROTO_NAMES})
  add_custom_command(
    OUTPUT ${GEN_DIR}/${name}.pb.cc ${GEN_DIR}/${name}.pb.h
    COMMAND ${PROTOC_EXE} --proto_path=${PROTO_DIR} --cpp_out=${GEN_DIR} ${PROTO_DIR}/${name}.proto
    DEPENDS ${PROTO_DIR}/${name}.proto
    COMMENT "protoc ${name}.proto"
    VERBATIM)
  list(APPEND PROTO_SRCS ${GEN_DIR}/${name}.pb.cc)
endforeach()
add_library(usp_proto STATIC ${PROTO_SRCS})
target_include_directories(usp_proto PUBLIC ${GEN_DIR})
target_link_libraries(usp_proto PUBLIC protobuf::libprotobuf)

# ---- Core library ----
add_library(usp_core STATIC
  src/common/Log.cpp
  src/common/Config.cpp
  src/stomp/StompFrame.cpp
  src/stomp/Transport.cpp
  src/stomp/StompClient.cpp
  src/usp/UspCodec.cpp
  src/controller/Controller.cpp
  src/controller/TaskApi.cpp
  src/controller/Cli.cpp)
target_include_directories(usp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(usp_core PUBLIC usp_proto Threads::Threads)
if(USP_WITH_TLS)
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(usp_core PUBLIC USP_WITH_TLS=1)
  target_link_libraries(usp_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_options(usp_core PRIVATE -Wall -Wextra)

add_executable(usp-controller src/main.cpp)
target_link_libraries(usp-controller PRIVATE usp_core)

add_executable(usp-fake-agent tools/usp_fake_agent.cpp)
target_link_libraries(usp-fake-agent PRIVATE usp_core)

install(TARGETS usp-controller usp-fake-agent RUNTIME DESTINATION bin)

if(USP_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE usp_core)
  add_test(NAME unit_tests COMMAND unit_tests)
endif()
