find_package(GTest REQUIRED)

if (WIN32)
  set(LIBRARIES asyncio-0.4 ws2_32 mswsock)
else()
  set(LIBRARIES asyncio-0.4 pthread)
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

SET_TARGET_PROPERTIES(udptest PROPERTIES
  COMPILE_FLAGS "-std=c++11"
)

target_link_libraries(unittest ${LIBRARIES} ${GTEST_LIBRARIES})
target_link_libraries(udptest ${LIBRARIES})
