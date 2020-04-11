add_subdirectory(unittest)
add_subdirectory(udptest)

if (ZMTP_ENABLED)
  add_subdirectory(zmtptest)
endif()
