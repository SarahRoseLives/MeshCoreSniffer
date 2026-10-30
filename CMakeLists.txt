cmake_minimum_required(VERSION 3.16)
project(MeshCoreSniffer C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(MESHCORE_CORE_SOURCES
    src/lora.cpp
    src/meshcore.cpp
    src/rtl_sdr.cpp
    src/ed25519/fe.c
    src/ed25519/ge.c
    src/ed25519/sc.c
    src/ed25519/sha512.c
    src/ed25519/verify.c
)

# ---- CLI (unchanged behavior) ----
add_executable(MeshCoreSniffer
    src/main.cpp
    ${MESHCORE_CORE_SOURCES}
)
target_include_directories(MeshCoreSniffer PRIVATE src src/ed25519)

if(MINGW)
  target_link_options(MeshCoreSniffer PRIVATE -static)
endif()

if(CMAKE_DL_LIBS)
  target_link_libraries(MeshCoreSniffer PRIVATE ${CMAKE_DL_LIBS})
endif()

# ---- GUI (Dear ImGui fixed dashboard) ----
option(MESHCORE_BUILD_GUI "Build the MeshCoreSniffer GUI" OFF)
if(MESHCORE_BUILD_GUI AND WIN32)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
  pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

  add_library(mc_imgui STATIC
      third_party/imgui/imgui.cpp
      third_party/imgui/imgui_draw.cpp
      third_party/imgui/imgui_tables.cpp
      third_party/imgui/imgui_widgets.cpp
      third_party/imgui/backends/imgui_impl_glfw.cpp
      third_party/imgui/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(mc_imgui PUBLIC
      third_party/imgui
      third_party/imgui/backends
  )
  target_link_libraries(mc_imgui PUBLIC PkgConfig::GLFW opengl32)

  add_executable(MeshCoreSnifferGUI WIN32
      src/gui/main_gui.cpp
      src/gui/app.cpp
      src/gui/panels.cpp
      src/gui/receiver.cpp
      src/gui/store.cpp
      src/gui/tiles.cpp
      ${MESHCORE_CORE_SOURCES}
  )
  target_include_directories(MeshCoreSnifferGUI PRIVATE
      src
      src/ed25519
      src/gui
      third_party/stb
  )
  target_link_libraries(MeshCoreSnifferGUI PRIVATE
      mc_imgui
      PkgConfig::SQLITE3
      opengl32
      winhttp
      comdlg32
      shell32
  )
  set_target_properties(MeshCoreSnifferGUI PROPERTIES OUTPUT_NAME "MeshCoreSniffer-gui")
  if(MINGW)
    target_link_options(MeshCoreSnifferGUI PRIVATE -static)
  endif()
endif()
