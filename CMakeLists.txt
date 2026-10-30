cmake_minimum_required(VERSION 3.16)
project(MeshCoreSniffer C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(MeshCoreSniffer
    src/main.cpp
    src/lora.cpp
    src/meshcore.cpp
    src/rtl_sdr.cpp
    src/ed25519/fe.c
    src/ed25519/ge.c
    src/ed25519/sc.c
    src/ed25519/sha512.c
    src/ed25519/verify.c
)
target_include_directories(MeshCoreSniffer PRIVATE src src/ed25519)

if(MINGW)
  target_link_options(MeshCoreSniffer PRIVATE -static)
endif()

# librtlsdr is loaded at runtime, so only dl is needed on POSIX.
if(CMAKE_DL_LIBS)
  target_link_libraries(MeshCoreSniffer PRIVATE ${CMAKE_DL_LIBS})
endif()
