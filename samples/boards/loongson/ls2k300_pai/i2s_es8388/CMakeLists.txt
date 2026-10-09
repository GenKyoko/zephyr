# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(i2s_es8388)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The sample audio is generated at build time: the built in melody by default,
# or the recording SAMPLE_AUDIO_INPUT points at (see tools/make_sample_audio.py).
# Nothing generated is kept in the tree.
set(SAMPLE_AUDIO_C ${CMAKE_CURRENT_BINARY_DIR}/sample_audio.c)
set(SAMPLE_AUDIO_H ${CMAKE_CURRENT_BINARY_DIR}/sample_audio.h)

# CMake drops empty arguments, so only pass --input when there is one
if(SAMPLE_AUDIO_INPUT)
  set(SAMPLE_AUDIO_ARGS --input "${SAMPLE_AUDIO_INPUT}")
else()
  set(SAMPLE_AUDIO_ARGS "")
endif()

add_custom_command(
  OUTPUT ${SAMPLE_AUDIO_C} ${SAMPLE_AUDIO_H}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_sample_audio.py
          --output-dir ${CMAKE_CURRENT_BINARY_DIR} ${SAMPLE_AUDIO_ARGS}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_sample_audio.py ${SAMPLE_AUDIO_INPUT}
  COMMENT "Generating the sample audio for the I2S + ES8388 test"
)

target_sources(app PRIVATE src/main.c ${SAMPLE_AUDIO_C})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Bring-up switches, both off by default:
#
#   -DSAMPLE_LIVE=1          keep replaying the melody, so the codec can be poked
#                            over I2C from the shell while audio flows
#   -DSAMPLE_CODEC_MASTER=1  let the codec generate BCLK and LRCLK (its own
#                            12.288 MHz oscillator clocks it); the board device
#                            tree must then set "slave-mode" on the I2S node
if(SAMPLE_LIVE)
  target_compile_definitions(app PRIVATE SAMPLE_LIVE=1)
endif()

if(SAMPLE_CODEC_MASTER)
  target_compile_definitions(app PRIVATE SAMPLE_CODEC_MASTER=1)
endif()

# -DSAMPLE_CLOCK_ONLY=1 generates the clocks but transmits nothing, so the
# capture phase reports the codec's own ADC output: a non-zero peak there means
# the codec is clocked and its data path works
if(SAMPLE_CLOCK_ONLY)
  target_compile_definitions(app PRIVATE SAMPLE_CLOCK_ONLY=1)
endif()
