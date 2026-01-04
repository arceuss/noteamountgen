cmake_minimum_required(VERSION 3.16)
project(SightRead VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library sources (excluding qbmidi which requires Boost)
set(SIGHTREAD_SOURCES
    src/sightread/chartparser.cpp
    src/sightread/midiparser.cpp
    src/sightread/song.cpp
    src/sightread/songparts.cpp
    src/sightread/tempomap.cpp
    src/sightread/detail/chart.cpp
    src/sightread/detail/chartconverter.cpp
    src/sightread/detail/midi.cpp
    src/sightread/detail/midiconverter.cpp
    src/sightread/detail/parserutil.cpp
)

# Create library
add_library(sightread STATIC ${SIGHTREAD_SOURCES})

target_include_directories(sightread PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(sightread PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(sightread PUBLIC cxx_std_20)

# Optional: Build tests
option(SIGHTREAD_BUILD_TESTS "Build SightRead tests" OFF)

if(SIGHTREAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
