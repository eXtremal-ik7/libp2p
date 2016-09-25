cmake_minimum_required(VERSION 2.8)

set(CMAKE_CXX_FLAGS "-std=c++11")

if (WIN32)
  include_directories(${CMAKE_SOURCE_DIR}/msvc/include)
  set(OS_WINDOWS 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(OS_LINUX 1)
  set(OS_COMMONUNIX 1)
elseif (APPLE)
  set(OS_DARWIN 1)
  set(OS_COMMONUNIX 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(OS_FREEBSD 1) 
  set(OS_COMMONUNIX 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(OS_QNX 1) 
  set(OS_COMMONUNIX 1)
endif()

if(MSVC)
  add_definitions(
    -D_CRT_SECURE_NO_DEPRECATE
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_DEPRECATE
  )
else(MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
  set (CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -Wall")
endif(MSVC)

include(TestBigEndian)
TEST_BIG_ENDIAN(IS_BIGENDIAN)
configure_file(
  ${CMAKE_SOURCE_DIR}/include/config.h.in
  ${CMAKE_BINARY_DIR}/include/config.h
)

include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/config4cpp/include
  ${CMAKE_BINARY_DIR}/include
)

option(SSL_ENABLED "SSL support (OpenSSL is required)" ON)
option(HTTP_ENABLED "HTTP protocol support" ON)

add_subdirectory(asyncio)
add_subdirectory(p2putils)
add_subdirectory(p2p)
add_subdirectory(examples)
