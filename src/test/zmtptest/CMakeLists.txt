include_directories(${ZMQ_INCLUDE_DIRECTORY})

if (WIN32)
  set(LIBRARIES asyncio-0.4 p2p ws2_32 mswsock ${ZMQ_LIBRARY})
else()
  set(LIBRARIES asyncio-0.4 p2p ${ZMQ_LIBRARY})
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

add_executable(zmtptest
  zmtptest.cpp
)

target_link_libraries(zmtptest ${LIBRARIES})
