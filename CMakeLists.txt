cmake_minimum_required(VERSION 3.29.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_DEBUG_POSTFIX "_d")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


include(cmake/get_cpm.cmake)

CPMFindPackage(
        NAME Hermes
        GITHUB_REPOSITORY NotRiemannCousin/Hermes
        GIT_TAG v0.4.1
)


add_executable(LoadBalancer
        src/LoadBalancer/main.cpp
        src/LoadBalancer/SocketForwarderAcceptPolicy.hpp
        src/LoadBalancer/SocketForwarderAcceptPolicy.cpp
        src/LoadBalancer/SocketForwarderData.cpp
        src/LoadBalancer/SocketForwarderData.hpp
)
add_executable(Api
        src/Api/main.cpp
        src/Api/SocketReceiverAcceptPolicy.cpp
        src/Api/SocketReceiverAcceptPolicy.hpp
        src/Api/SocketReceiverData.cpp
        src/Api/SocketReceiverData.hpp
)

target_link_libraries(LoadBalancer PRIVATE Hermes)
target_link_libraries(Api PRIVATE Hermes)

