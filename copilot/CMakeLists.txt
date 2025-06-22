cmake_minimum_required(VERSION 3.10)

project(TCPNetworkingLibrary)

# Set the C++ standard (C++11 in this case)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Boost (we are using Boost.Asio which requires at least Boost.System)
find_package(Boost REQUIRED COMPONENTS system)
# Find threads (for std::thread and potential pthread linking)
find_package(Threads REQUIRED)

# Include the Boost includes
include_directories(${Boost_INCLUDE_DIRS})
# Optionally include the current source directory if needed for header files
include_directories(${CMAKE_SOURCE_DIR})

# List files to compile; header files do not need to be explicitly compiled.
set(SOURCES
    main.cpp
    Session.cpp
    TCPClient.cpp
    TCPServer.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

# Link against Boost.System and Threads libraries.
target_link_libraries(${PROJECT_NAME}
    Boost::system
    Threads::Threads
)
