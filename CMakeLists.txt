cmake_minimum_required(VERSION 3.16)
project(MeshCoreSniffer C CXX)

if(APPLE AND EXISTS "/opt/local")
  list(APPEND CMAKE_PREFIX_PATH "/opt/local")
  if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "\$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    set(ENV{PKG_CONFIG_PATH} "\$ENV{PKG_CONFIG_PATH}:/opt/local/lib/pkgconfig")
  else()
    set(ENV{PKG_CONFIG_PATH} "/opt/local/lib/pkgconfig")
  endif()
endif()

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
if(MESHCORE_BUILD_GUI)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
  pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
  find_package(Threads REQUIRED)
  if(WIN32)
    set(MESHCORE_OPENGL_LIBS opengl32)
  else()
    find_package(OpenGL REQUIRED)
    set(MESHCORE_OPENGL_LIBS OpenGL::GL)
  endif()

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
  target_link_libraries(mc_imgui PUBLIC PkgConfig::GLFW ${MESHCORE_OPENGL_LIBS})

  add_executable(MeshCoreSnifferGUI
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
      Threads::Threads
      ${MESHCORE_OPENGL_LIBS}
  )

  if(APPLE)
    target_compile_definitions(MeshCoreSnifferGUI PRIVATE GL_SILENCE_DEPRECATION)
  endif()

  if(WIN32)
    target_link_libraries(MeshCoreSnifferGUI PRIVATE
        winhttp
        comdlg32
        shell32
    )
  else()
    if(CMAKE_DL_LIBS)
      target_link_libraries(MeshCoreSnifferGUI PRIVATE ${CMAKE_DL_LIBS})
    endif()
    pkg_check_modules(CURL QUIET IMPORTED_TARGET libcurl)
    if(CURL_FOUND)
      target_link_libraries(MeshCoreSnifferGUI PRIVATE PkgConfig::CURL)
      target_compile_definitions(MeshCoreSnifferGUI PRIVATE MESHCORE_HAVE_CURL=1)
    endif()
  endif()

  set_target_properties(MeshCoreSnifferGUI PROPERTIES OUTPUT_NAME "MeshCoreSniffer-gui")
  if(WIN32)
    set_target_properties(MeshCoreSnifferGUI PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()
  if(MINGW)
    target_link_options(MeshCoreSnifferGUI PRIVATE -static)
  endif()
endif()