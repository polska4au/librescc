# Unit tests for the non-GUI, non-audio-hardware-dependent parts of LibreSCC:
# the MIDI parser, the synth/voice-stealing engine, and the playback
# scheduler. These link against the real synth engine but never open a
# PortAudio stream, so they run fine in headless CI containers.

add_executable(test_midi_parser
    ${CMAKE_SOURCE_DIR}/tests/test_midi_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/midi_file.cpp
)
target_include_directories(test_midi_parser PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(test_synth_engine
    ${CMAKE_SOURCE_DIR}/tests/test_synth_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/synth_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/instrument_map.cpp
)
target_include_directories(test_synth_engine PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_synth_engine PRIVATE Threads::Threads)

add_executable(test_player
    ${CMAKE_SOURCE_DIR}/tests/test_player.cpp
    ${CMAKE_SOURCE_DIR}/src/player.cpp
    ${CMAKE_SOURCE_DIR}/src/midi_file.cpp
    ${CMAKE_SOURCE_DIR}/src/synth_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/instrument_map.cpp
)
target_include_directories(test_player PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_player PRIVATE Threads::Threads)

add_test(NAME midi_parser WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND test_midi_parser)
add_test(NAME synth_engine WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND test_synth_engine)
add_test(NAME player WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND test_player)
