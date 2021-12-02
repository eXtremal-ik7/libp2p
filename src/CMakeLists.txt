include("cmake/HunterGate.cmake")
HunterGate(
    URL "https://github.com/cpp-pm/hunter/archive/v0.23.318.tar.gz"
    SHA1 "0475be76dbf51385790bbe96be57279e2ff0861d"
)

project(libp2p)
cmake_minimum_required(VERSION 2.8)

set (CMAKE_CXX_STANDARD 11)
option(SSL_ENABLED "SSL support (OpenSSL is required)" ON)
option(ZMTP_ENABLED "ZMTP (zmq) protocol support" ON)
option(BTC_ENABLED "Bitcoin network protocol support" ON)
option(RLPX_ENABLED "RPLx (Ethereum) network protocol support" ON)
option(TEST_ENABLED "Build tests" OFF)
option(SANITIZER_ENABLED "Build with address sanitizer" OFF)
option(PROFILE_ENABLED "Build for profiling" OFF)

if (SANITIZER_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if (PROFILE_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
endif()

if (SSL_ENABLED OR BTC_ENABLED)
  hunter_add_package(OpenSSL)
  find_package(OpenSSL REQUIRED)
endif()

if (ZMTP_ENABLED)
  hunter_add_package(ZeroMQ)
  find_package(ZeroMQ CONFIG REQUIRED)
  set(Sources ${Sources} zmtp.cpp)
endif()

set(CMAKE_DEBUG_POSTFIX d)

if (WIN32)
  if(MSVC)
    SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/msvc/include)
  endif(MSVC)
  set(OS_WINDOWS 1)
  add_definitions(
    -D__STDC_LIMIT_MACROS
    -D__STDC_FORMAT_MACROS
  )
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

if (CMAKE_OSX_ARCHITECTURES)
  set(ARCH ${CMAKE_OSX_ARCHITECTURES})
else()
  set(ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()

if (ARCH STREQUAL "i386" OR ARCH STREQUAL "i686")
  set(ARCH_X86 1)
  set(ARCH_NAME "x86")
elseif (ARCH STREQUAL "x86_64" OR ARCH STREQUAL "AMD64")
  set(ARCH_X86_64 1)
  set(ARCH_NAME "x86_64")
elseif (ARCH STREQUAL "aarch64" OR ARCH STREQUAL "arm64")
  set(ARCH_AARCH64 1)
  set(ARCH_NAME "aarch64")
else()
  message(FATAL_ERROR "Unsupported processor architecture")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OS_64 1)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(OS_32 1)
endif()

if(MSVC)
  add_definitions(
    -D_CRT_SECURE_NO_DEPRECATE
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_DEPRECATE
  )
else(MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif(MSVC)

include(TestBigEndian)
TEST_BIG_ENDIAN(IS_BIGENDIAN)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/libp2pconfig.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/libp2pconfig.h
)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/config4cpp/include
  ${CMAKE_CURRENT_BINARY_DIR}/include
)

add_subdirectory(asyncio)
add_subdirectory(asyncioextras)
add_subdirectory(p2putils)
add_subdirectory(p2p)
add_subdirectory(examples)

if (TEST_ENABLED)
  add_subdirectory(test)
endif()
