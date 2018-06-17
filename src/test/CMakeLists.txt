find_package(GTest REQUIRED)

add_executable(unittest
  unittest.cpp
)

include_directories(unittest
  ${GTEST_INCLUDE_DIRS}
)

target_link_libraries(unittest asyncio-0.4 ${GTEST_LIBRARIES})
if (WIN32)
  target_link_libraries(unittest ws2_32 mswsock)
else()
  target_link_libraries(unittest pthread)
endif()
