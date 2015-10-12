set(Sources asyncio.c
  cstl.c
  objectPool.c
  dynamicBuffer.c
  timer.c
)

if (SSL_ENABLED)
  set(Sources ${Sources} socketSSL.c)
endif()

if (HTTP_ENABLED)
  set(Sources ${Sources} http.c)
endif()

if (WIN32)
  set(Sources ${Sources} iocp.c deviceWin32.c socketWin32.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(Sources ${Sources} select.cpp epoll.c devicePosix.c socketPosix.c)
elseif (APPLE)
  set(Sources ${Sources} kqueue.cpp devicePosix.c socketPosix.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(Sources ${Sources} select.cpp kqueue.cpp devicePosix.c socketPosix.c)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_definitions(-fPIC)
endif ()

add_library(asyncio-0.1pre6 STATIC ${Sources})
if (HTTP_ENABLED)
  add_dependencies(asyncio-0.1pre6 p2putils)
endif()

if (NOT WIN32 AND NOT APPLE)
  target_link_libraries(asyncio-0.1pre6 rt)
endif()

install(
  TARGETS asyncio-0.1pre6
  ARCHIVE DESTINATION lib
)
