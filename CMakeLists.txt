cmake_minimum_required(VERSION 3.10)
project(TCP_MIDI_Client)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Libremidi)

if(NOT Libremidi_FOUND)
  #Fetch libremidi from GitHub
  include(FetchContent)

  FetchContent_Declare(
      libremidi
      GIT_REPOSITORY https://github.com/celtera/libremidi.git
      GIT_TAG master
  )

  # This will clone and add_subdirectory(libremidi)
  FetchContent_MakeAvailable(libremidi)
endif()

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC
    SDL2::SDL2
    OpenGL::GL
)

add_executable(TCP_MIDI_Client
    main.cpp
    AppController.cpp
    clockState.cpp
    GuiApp.cpp
    transportClient.cpp
    timeSync.cpp
    receiveScheduler.cpp
    midiInputHandler.cpp
    midiOutputHandler.cpp
    recoveryJournal.cpp
)

target_include_directories(TCP_MIDI_Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(TCP_MIDI_Client PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    libremidi
    imgui
    SDL2::SDL2
    OpenGL::GL
)

target_compile_definitions(TCP_MIDI_Client PRIVATE LIBREMIDI_HEADER_ONLY=1)

message(STATUS "=== Modular TCP-MIDI TCP_MIDI_Client configured ===")
