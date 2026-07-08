cmake_minimum_required(VERSION 3.16)
project(librescc LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

# Optional MP3 export support: only enabled if libmp3lame is found at
# configure time. If absent, the app still builds fine and the MP3 export
# button in the UI is simply disabled at runtime (see src/ui.cpp).
find_path(LAME_INCLUDE_DIR lame/lame.h)
find_library(LAME_LIBRARY mp3lame)
if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    set(LIBRESCC_HAVE_LAME TRUE)
    message(STATUS "libmp3lame found: MP3 export enabled (${LAME_LIBRARY})")
else()
    set(LIBRESCC_HAVE_LAME FALSE)
    message(STATUS "libmp3lame not found: MP3 export button will be disabled")
endif()

set(LIBRESCC_SOURCES
    src/main.cpp
    src/synth_engine.cpp
    src/audio_engine.cpp
    src/midi_file.cpp
    src/player.cpp
    src/instrument_map.cpp
    src/ui.cpp
)

add_executable(librescc ${LIBRESCC_SOURCES})

target_include_directories(librescc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GTK4_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE_DIRS}
)

target_compile_options(librescc PRIVATE
    ${GTK4_CFLAGS_OTHER}
    ${PORTAUDIO_CFLAGS_OTHER}
    -Wall -Wextra
)

target_link_directories(librescc PRIVATE ${GTK4_LIBRARY_DIRS} ${PORTAUDIO_LIBRARY_DIRS})

target_link_libraries(librescc PRIVATE
    ${GTK4_LIBRARIES}
    ${PORTAUDIO_LIBRARIES}
    Threads::Threads
)

if(LIBRESCC_HAVE_LAME)
    target_compile_definitions(librescc PRIVATE LIBRESCC_HAVE_LAME=1)
    target_include_directories(librescc PRIVATE ${LAME_INCLUDE_DIR})
    target_link_libraries(librescc PRIVATE ${LAME_LIBRARY})
endif()

install(TARGETS librescc RUNTIME DESTINATION bin)
install(FILES man/librescc.1 DESTINATION share/man/man1 OPTIONAL)
install(FILES ${CMAKE_SOURCE_DIR}/assets/librescc.desktop DESTINATION share/applications OPTIONAL)

enable_testing()
add_subdirectory(tests)
