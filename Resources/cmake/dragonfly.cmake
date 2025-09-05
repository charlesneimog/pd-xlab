add_subdirectory(Sources/vst3/dragonfly/dpf/ EXCLUDE_FROM_ALL)

# ╭──────────────────────────────────────╮
# │            FREEVERB_SRCS             │
# ╰──────────────────────────────────────╯
include_directories(Sources/vst3/dragonfly/common)
set(FREEVERB_SRCS
    Sources/vst3/dragonfly/common/kiss_fft/kiss_fft.c
    Sources/vst3/dragonfly/common/kiss_fft/kiss_fftr.c
    Sources/vst3/dragonfly/common/freeverb/allpass.cpp
    Sources/vst3/dragonfly/common/freeverb/biquad.cpp
    Sources/vst3/dragonfly/common/freeverb/comb.cpp
    Sources/vst3/dragonfly/common/freeverb/delay.cpp
    Sources/vst3/dragonfly/common/freeverb/delayline.cpp
    Sources/vst3/dragonfly/common/freeverb/earlyref.cpp
    Sources/vst3/dragonfly/common/freeverb/efilter.cpp
    Sources/vst3/dragonfly/common/freeverb/nrev.cpp
    Sources/vst3/dragonfly/common/freeverb/nrevb.cpp
    Sources/vst3/dragonfly/common/freeverb/progenitor.cpp
    Sources/vst3/dragonfly/common/freeverb/progenitor2.cpp
    Sources/vst3/dragonfly/common/freeverb/revbase.cpp
    Sources/vst3/dragonfly/common/freeverb/slot.cpp
    Sources/vst3/dragonfly/common/freeverb/strev.cpp
    Sources/vst3/dragonfly/common/freeverb/utils.cpp
    Sources/vst3/dragonfly/common/freeverb/zrev.cpp
    Sources/vst3/dragonfly/common/freeverb/zrev2.cpp)

add_library(freeverb_static STATIC "${FREEVERB_SRCS}")
set_target_properties(
  freeverb_static
  PROPERTIES CXX_STANDARD 11
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS ON)
target_compile_definitions(freeverb_static PUBLIC LIBFV3_FLOAT)
set_target_properties(freeverb_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ╭──────────────────────────────────────╮
# │     Dragonfly-early-reflections      │
# ╰──────────────────────────────────────╯
set(FILES_DSP_dragonfly-early
    Sources/vst3/dragonfly/plugins/dragonfly-early-reflections/Plugin.cpp)
set(FILES_UI_dragonfly-early
    Sources/vst3/dragonfly/plugins/dragonfly-early-reflections/Artwork.cpp
    Sources/vst3/dragonfly/plugins/dragonfly-early-reflections/UI.cpp
    Sources/vst3/dragonfly/plugins/dragonfly-early-reflections/AbstractUI.cpp
    Sources/vst3/dragonfly/plugins/dragonfly-early-reflections/Artwork.cpp
    Sources/vst3/dragonfly/common/LabelledKnob.cpp
    Sources/vst3/dragonfly/common/Selection.cpp
    Sources/vst3/dragonfly/common/Bitstream_Vera_Sans_Regular.cpp)

dpf_add_plugin(
  dragonfly-early-reflections
  TARGETS
  vst3
  FILES_DSP
  "${FILES_DSP_dragonfly-early};Sources/vst3/dragonfly/plugins/dragonfly-early-reflections/DSP.cpp"
  FILES_UI
  "${FILES_UI_dragonfly-early}")
target_include_directories(
  dragonfly-early-reflections
  PUBLIC "Sources/vst3/dragonfly/plugins/dragonfly-early-reflections/")

set_target_properties(
  dragonfly-early-reflections-dsp
  PROPERTIES CXX_STANDARD 11
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS ON)
set_target_properties(
  dragonfly-early-reflections-ui
  PROPERTIES CXX_STANDARD 11
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS ON)
target_link_libraries(dragonfly-early-reflections PUBLIC freeverb_static)

# ╭──────────────────────────────────────╮
# │        Dragonfly-room-reverb         │
# ╰──────────────────────────────────────╯
set(FILES_DSP_dragonfly-early
    Sources/vst3/dragonfly/plugins/dragonfly-room-reverb/Plugin.cpp)
set(FILES_UI_dragonfly-early
    Sources/vst3/dragonfly/plugins/dragonfly-room-reverb/Artwork.cpp
    Sources/vst3/dragonfly/plugins/dragonfly-room-reverb/UI.cpp
    Sources/vst3/dragonfly/plugins/dragonfly-room-reverb/AbstractUI.cpp
    Sources/vst3/dragonfly/plugins/dragonfly-room-reverb/Artwork.cpp
    Sources/vst3/dragonfly/common/LabelledKnob.cpp
    Sources/vst3/dragonfly/common/Selection.cpp
    Sources/vst3/dragonfly/common/Spectrogram.cpp
    Sources/vst3/dragonfly/common/Bitstream_Vera_Sans_Regular.cpp)

dpf_add_plugin(
  dragonfly-room-reverb
  TARGETS
  vst3
  FILES_DSP
  "${FILES_DSP_dragonfly-early};Sources/vst3/dragonfly/plugins/dragonfly-room-reverb/DSP.cpp"
  FILES_UI
  "${FILES_UI_dragonfly-early}")
target_include_directories(
  dragonfly-room-reverb
  PUBLIC "Sources/vst3/dragonfly/plugins/dragonfly-room-reverb/")

set_target_properties(
  dragonfly-room-reverb-dsp
  PROPERTIES CXX_STANDARD 11
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS ON)
set_target_properties(
  dragonfly-room-reverb-ui
  PROPERTIES CXX_STANDARD 11
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS ON)
target_link_libraries(dragonfly-room-reverb PUBLIC freeverb_static)
