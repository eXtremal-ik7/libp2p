project(asyncio C ASM)

set(Sources
  asyncio.c
  asyncioImpl.c
  dynamicBuffer.c
  ringBuffer.c
  timer.c

  http.c
  smtp.c

  base64.c
)

if (SSL_ENABLED)
  include_directories(${OPENSSL_INCLUDE_DIRECTORY})
  set(Sources ${Sources} socketSSL.c)
endif()

if (WIN32)
  set(Sources ${Sources} iocp.c coroutineWin32.c deviceWin32.c socketWin32.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(Sources ${Sources} select.c epoll.c devicePosix.c socketPosix.c coroutinePosix.c ${ARCH_NAME}/posix.s)
elseif (APPLE)
  set(Sources ${Sources} kqueue.c devicePosix.c socketPosix.c coroutinePosix.c ${ARCH_NAME}/posix.s)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(Sources ${Sources} select.c kqueue.c devicePosix.c socketPosix.c coroutinePosix.c ${ARCH_NAME}/posix.s)
elseif(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(Sources ${Sources} select.c devicePosix.c socketPosix.c coroutinePosix.c ${ARCH_NAME}/posix.s)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif ()

add_library(asyncio-0.5 STATIC ${Sources})
if (HTTP_ENABLED)
  add_dependencies(asyncio-0.5 p2putils)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  target_link_libraries(asyncio-0.5 rt)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "QNX")
  target_link_libraries(asyncio-0.4 socket)
endif()

install(
  TARGETS asyncio-0.5
  ARCHIVE DESTINATION lib
)
