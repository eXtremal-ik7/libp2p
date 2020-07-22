if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_definitions(-fPIC)
endif ()

add_library(p2putils
  LiteFlatHashTable.c
  HttpParse.cpp
  HttpRequestParse.cpp
  UriParse.cpp
)

install(
  TARGETS p2putils
  ARCHIVE DESTINATION lib
)
