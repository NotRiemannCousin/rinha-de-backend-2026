cmake_minimum_required(VERSION 3.29.1)
project("Rinha de Backend" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_DEBUG_POSTFIX "_d")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


set(CMAKE_COLOR_DIAGNOSTICS ON)
add_compile_options(-fdiagnostics-color=always)



add_compile_options("$<$<CONFIG:Release>:-O3;-march=haswell;-ffast-math>"  -fcontracts)
add_link_options(-fcontracts)

include(cmake/get_cpm.cmake)

CPMFindPackage(
        NAME Hermes
        GITHUB_REPOSITORY NotRiemannCousin/Hermes
        GIT_TAG v0.5.4-beta
)
CPMFindPackage(
        NAME simdjson
        GITHUB_REPOSITORY simdjson/simdjson
        VERSION 3.10.0
        OPTIONS "SIMDJSON_DEVELOPER_MODE OFF" "SIMDJSON_JUST_HEADERS ON"
)

set_target_properties(Hermes PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
target_compile_options(Hermes PRIVATE $<$<CONFIG:Release>:-fopenmp>)
target_link_options(Hermes PRIVATE $<$<CONFIG:Release>:-fopenmp>)


include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)


add_executable(LoadBalancer
        src/LoadBalancer/SocketForwarder/SocketForwarderAcceptPolicy.cpp
        src/LoadBalancer/SocketForwarder/SocketForwarderData.cpp

        src/LoadBalancer/main.cpp
)
target_link_libraries(LoadBalancer PUBLIC Hermes)
set_target_properties(LoadBalancer PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(Api
        src/Api/SocketReceiver/SocketReceiverAcceptPolicy.cpp
        src/Api/SocketReceiver/SocketReceiverData.cpp
        src/Api/Response.cpp
        src/Api/Dataset.cpp
        src/Api/Search.cpp

        src/Api/main.cpp
)
target_link_libraries(Api PUBLIC Hermes simdjson)
set_target_properties(Api PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)