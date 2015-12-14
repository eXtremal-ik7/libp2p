set(EchoServerSources EchoServer.cpp)
set(TelnetClientSources TelnetClient.cpp)
set(AIOPingSources AIOPing.cpp)
set(P2PClusterSources P2PCluster.cpp)

if (WIN32)
  set(AIOPingSources ${AIOPingSources}
    ${CMAKE_SOURCE_DIR}/msvc/getopt.c
    ${CMAKE_SOURCE_DIR}/msvc/getopt1.c
  )
endif()

add_executable(EchoServer ${EchoServerSources})
add_executable(TelnetClient ${TelnetClientSources})
add_executable(aioping ${AIOPingSources})
add_executable(p2pcluster ${P2PClusterSources})

target_link_libraries(EchoServer asyncio-0.1pre6)
target_link_libraries(TelnetClient asyncio-0.1pre6)
target_link_libraries(aioping asyncio-0.1pre6)
target_link_libraries(p2pcluster p2pcore)
if (WIN32)
  target_link_libraries(EchoServer ws2_32 mswsock)
  target_link_libraries(TelnetClient ws2_32 mswsock)
  target_link_libraries(aioping ws2_32 mswsock)
endif()
