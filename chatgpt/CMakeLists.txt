cmake_minimum_required(VERSION 3.10)

project(TCPNetworkingLibrary)

# Set the C++ standard (C++11 in this case)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

    set(CMAKE_BUILD_TYPE "Debug")

# Find Boost (we are using Boost.Asio which requires at least Boost.System)
find_package(Boost REQUIRED COMPONENTS system)
# Find threads (for std::thread and potential pthread linking)
find_package(Threads REQUIRED)

# Include the Boost includes
include_directories(${Boost_INCLUDE_DIRS} include/)
# Optionally include the current source directory if needed for header files
include_directories(${CMAKE_SOURCE_DIR})

add_executable(client
               exp_client.cpp

               include/client.cpp
               include/connection.cpp
)

target_link_libraries(client
    Boost::system
    Threads::Threads
)

add_executable(server
               exp_server.cpp

               include/server.cpp
               include/connection.cpp
)

target_link_libraries(server
    Boost::system
    Threads::Threads
)


# add_executable(perf
#     performance.cpp
#     Session.cpp
#     TCPClient.cpp
#     TCPServer.cpp
# )

# # Link against Boost.System and Threads libraries.
# target_link_libraries(perf
#     Boost::system
#     Threads::Threads
# )
