set(EchoServerSources EchoServer.cpp)
set(EchoServerSources_coro EchoServer_coro.cpp)

add_executable(EchoServer ${EchoServerSources})
add_executable(EchoServer_coro ${EchoServerSources_coro})

target_link_libraries(EchoServer asyncio-0.5)
target_link_libraries(EchoServer_coro asyncio-0.5)
if (WIN32)
  target_link_libraries(EchoServer ws2_32 mswsock)
  target_link_libraries(EchoServer_coro ws2_32 mswsock)  
endif()
