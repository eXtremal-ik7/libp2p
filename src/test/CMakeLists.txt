set(CMAKE_CXX_STANDARD 11)

if (ZMTP_ENABLED)
  find_path(ZMQ_INCLUDE_DIRECTORY zmq.h)
  find_library(ZMQ_LIBRARY zmq)
endif()

add_subdirectory(unittest)
add_subdirectory(udptest)

if (ZMTP_ENABLED)
  add_subdirectory(zmtptest)
endif()
