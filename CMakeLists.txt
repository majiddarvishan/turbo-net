# cmake_minimum_required(VERSION 3.14)
# project(TurboNetClient)

# # Enable C++17
# set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# # Enable CPM
# include(FetchContent)
# FetchContent_Declare(
#   CPM
#   GIT_REPOSITORY https://github.com/cpm-cmake/CPM.cmake
#   GIT_TAG origin/master
# )
# FetchContent_MakeAvailable(CPM)

# # Fetch standalone Asio via CPM
# CPMAddPackage(
#   NAME asio
#   GITHUB_REPOSITORY chriskohlhoff/asio
#   VERSION 1.34.2
#   GIT_TAG asio-1-34-2
#   OPTIONS "ASIO_NO_DEPRECATED=1"
#           "ASIO_STANDALONE ON"
# )

# # Create your executable
# add_executable(turbonet_example
#   main.cpp
#   turbonet_client.cpp
# )

# # Include headers
# target_include_directories(turbonet_example PRIVATE ${asio_SOURCE_DIR}/asio/include)
# target_compile_definitions(turbonet PUBLIC ASIO_STANDALONE)

# # Link asio
# target_link_libraries(turbonet_example PRIVATE asio)

# # Threads
# find_package(Threads REQUIRED)
# target_link_libraries(turbonet_example PRIVATE Threads::Threads)


cmake_minimum_required(VERSION 3.15)
project(TurboNetClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download and include CPM.cmake
include(FetchContent)
FetchContent_Declare(
  CPM
  GIT_REPOSITORY https://github.com/cpm-cmake/CPM.cmake
  GIT_TAG origin/master
)
FetchContent_MakeAvailable(CPM)

# Asio (standalone, header-only)
# CPMAddPackage(
#   NAME asio
#   GITHUB_REPOSITORY chriskohlhoff/asio
#   VERSION 1.34.2
#   GIT_TAG asio-1-34-2
#   OPTIONS "ASIO_STANDALONE ON"
# )

CPMAddPackage(
  NAME Boost
  VERSION 1.86.0
  GITHUB_REPOSITORY "boostorg/boost"
  GIT_TAG "boost-1.86.0"
)

add_library(turbonet
  turbonet_client.cpp
  turbonet_client.h
)

target_link_libraries(turbonet
                      PRIVATE
                      Boost::asio
                     )

# target_include_directories(turbonet PUBLIC ${asio_SOURCE_DIR}/asio/include)
# target_compile_definitions(turbonet PUBLIC ASIO_STANDALONE)

add_executable(example_client client.cpp)
target_link_libraries(example_client
                      turbonet
                      Boost::asio
                     )
