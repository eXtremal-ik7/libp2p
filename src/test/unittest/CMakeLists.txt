find_package(GTest REQUIRED)

set(SOURCES unittest.cpp)
set(LIBRARIES asyncio-0.4 asyncioextras-0.4 p2p ${GTEST_LIBRARIES})
include_directories(${GTEST_INCLUDE_DIRS})

if (ZMTP_ENABLED)
  set(SOURCES ${SOURCES} zmtptest.cpp)
  set(LIBRARIES ${LIBRARIES} ${ZMQ_LIBRARY})
  include_directories(${ZMQ_INCLUDE_DIRECTORY})
endif()

if (WIN32)
  set(LIBRARIES ${LIBRARIES} ws2_32 mswsock)
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

add_executable(unittest ${SOURCES})
target_link_libraries(unittest ${LIBRARIES})