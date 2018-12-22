project(asyncio C ASM)
set_source_files_properties(coroutinePosix.s PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp")

set(Sources
  asyncio.c
  asyncioImpl.c
  objectPool.c
  dynamicBuffer.c
  timer.c
)

if (SSL_ENABLED)
  include_directories(${OPENSSL_INCLUDE_DIR})
  set(Sources ${Sources} socketSSL.c)
endif()

if (HTTP_ENABLED)
  set(Sources ${Sources} http.c)
endif()

if (WIN32)
  set(Sources ${Sources} iocp.c coroutineWin32.c deviceWin32.c socketWin32.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(Sources ${Sources} select.c epoll.c devicePosix.c socketPosix.c coroutinePosix.c coroutinePosix.s)
elseif (APPLE)
  set(Sources ${Sources} kqueue.cpp devicePosix.c socketPosix.c coroutinePosix.c coroutinePosix.s)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(Sources ${Sources} select.c kqueue.cpp devicePosix.c socketPosix.c coroutinePosix.c coroutinePosix.s)
elseif(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(Sources ${Sources} select.c devicePosix.c socketPosix.c coroutinePosix.c coroutinePosix.s)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_definitions(-fPIC)
endif ()

add_library(asyncio-0.4 STATIC ${Sources})
if (HTTP_ENABLED)
  add_dependencies(asyncio-0.4 p2putils)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  target_link_libraries(asyncio-0.4 rt)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "QNX")
  target_link_libraries(asyncio-0.4 socket)
endif()

install(
  TARGETS asyncio-0.4
  ARCHIVE DESTINATION lib
)
