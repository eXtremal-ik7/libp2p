set(CMAKE_CXX_STANDARD 11)
find_package(GTest REQUIRED)

if (WIN32)
  set(LIBRARIES asyncio-0.4 p2p ws2_32 mswsock)
else()
  set(LIBRARIES asyncio-0.4 p2p)
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

add_executable(unittest
  unittest.cpp
)

add_executable(udptest
  udptest.cpp
)

include_directories(unittest
  ${GTEST_INCLUDE_DIRS}
)

target_link_libraries(unittest ${LIBRARIES} ${GTEST_LIBRARIES})
target_link_libraries(udptest ${LIBRARIES})
