set(AIOPingSources AIOPing.cpp)

if (WIN32)
  set(AIOPingSources ${AIOPingSources}
    ${CMAKE_SOURCE_DIR}/msvc/getopt.c
    ${CMAKE_SOURCE_DIR}/msvc/getopt1.c
  )
endif()

add_executable(aioping ${AIOPingSources})

target_link_libraries(aioping asyncio-0.3)
if (WIN32)
  target_link_libraries(aioping ws2_32 mswsock)
endif()
