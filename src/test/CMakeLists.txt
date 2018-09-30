if (WIN32)
  set(LIBRARIES asyncio-0.3 ws2_32 mswsock)
else()
  set(LIBRARIES asyncio-0.3 pthread)
endif()

add_executable(udptest
  udptest.cpp
)

SET_TARGET_PROPERTIES(udptest PROPERTIES
  COMPILE_FLAGS "-std=c++11"
)

target_link_libraries(udptest ${LIBRARIES})
