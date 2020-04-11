if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_definitions(-fPIC)
endif ()

add_library(p2p STATIC
  p2pformat.cpp
  p2pproto.cpp
  p2p.cpp
)
