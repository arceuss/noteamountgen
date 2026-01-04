cmake_minimum_required(VERSION 3.16)
project(NoteAmountGenGUI VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows GUI application (no console)
if(WIN32)
    set(CMAKE_WIN32_EXECUTABLE ON)
endif()

# Add SightRead library (as submodule)
add_subdirectory(${CMAKE_SOURCE_DIR}/SightRead ${CMAKE_BINARY_DIR}/sightread)

# JSON library for config/data
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Main GUI executable
add_executable(noteamountgen_gui WIN32
    src/main.cpp
    src/chart_writer.cpp
    src/loop_generator.cpp
    src/chart_info.cpp
)

target_include_directories(noteamountgen_gui PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/SightRead/include
)

target_link_libraries(noteamountgen_gui PRIVATE 
    sightread
    nlohmann_json::nlohmann_json
)

# Windows libraries for native GUI
if(WIN32)
    target_link_libraries(noteamountgen_gui PRIVATE 
        comctl32
        comdlg32
        ole32
        shell32
    )
endif()

# Install target
install(TARGETS noteamountgen_gui DESTINATION bin)
