cmake_minimum_required(VERSION 3.10)
project(KK4V)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread -Wno-incompatible-pointer-types -fno-delete-null-pointer-checks")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -fno-delete-null-pointer-checks")

# Add Oniguruma dependency
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static oniguruma" FORCE)
set(BUILD_TEST OFF CACHE BOOL "Build tests" FORCE)
add_subdirectory(vendor/oniguruma)

# Core include directories
include_directories(
    src
    src/core/tjs2
    src/core/base
    src/core/base/win32
    src/core/utils
    src/core/utils/win32
    src/core/msg
    src/core/msg/win32
    src/core/environ
    src/core
    src/core/environ/win32
    src/core/environ/sdl
    src/core/extension
    src/core/sound
    src/core/sound/win32
    src/core/visual
    src/core/visual/win32
    src/core/visual/gl
    vendor/oniguruma/src
    vendor
    $ENV{VITASDK}/arm-vita-eabi/include/freetype2
    $ENV{VITASDK}/arm-vita-eabi/include/opus
    $ENV{VITASDK}/arm-vita-eabi/include/SDL2
)

# Collect sources
file(GLOB TJS2_SOURCES "src/core/tjs2/*.cpp")

file(GLOB MSG_SOURCES "src/core/msg/*.cpp" "src/core/msg/win32/*.cpp")
list(REMOVE_ITEM MSG_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/msg/win32/MsgLoad.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/msg/win32/ReadOptionDesc.cpp"
)

file(GLOB UTILS_SOURCES "src/core/utils/*.cpp" "src/core/utils/*.c" "src/core/utils/encoding/*.c" "src/core/utils/win32/*.cpp" "src/core/utils/minizip/*.c" "src/core/utils/minizip/*.cpp")
file(GLOB BASE_SOURCES "src/core/base/*.cpp" "src/core/base/win32/*.cpp")
# Exclude some win32 files not used in POSIX ports and missing 7zip
list(REMOVE_ITEM BASE_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/base/win32/FuncStubs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/base/win32/SusieArchive.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/base/7zArchive.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/base/ZIPArchive.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/base/TARArchive.cpp"
)

# Create static libraries
add_library(tjs2 STATIC ${TJS2_SOURCES})
target_link_libraries(tjs2 onig)

add_library(msg STATIC ${MSG_SOURCES})
add_library(utils STATIC ${UTILS_SOURCES})
add_library(base STATIC ${BASE_SOURCES})

file(GLOB ENVIRON_SOURCES "src/core/environ/*.cpp" "src/core/environ/sdl/*.cpp")
list(APPEND ENVIRON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/core/environ/win32/SystemControl.cpp")
list(REMOVE_ITEM ENVIRON_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/environ/DumpSend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/environ/XP3ArchiveRepack.cpp"
)
file(GLOB SOUND_SOURCES "src/core/sound/*.cpp" "src/core/sound/win32/*.cpp")
list(REMOVE_ITEM SOUND_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/sound/xmmlib.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/sound/WaveFormatConverter_SSE.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/sound/FFWaveDecoder.cpp"
)

file(GLOB VISUAL_SOURCES "src/core/visual/*.cpp" "src/core/visual/win32/*.cpp" "src/core/visual/gl/*.cpp")
list(APPEND VISUAL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xxhash/xxhash.c")
list(APPEND VISUAL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lz4/lz4.c")
list(REMOVE_ITEM VISUAL_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/win32/GDIFontRasterizer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/win32/NativeFreeTypeFace.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/win32/TVPSysFont.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/win32/VSyncTimingThread.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/LoadBPG.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/LoadJXR.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/LoadPVRv2.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual/LoadPVRv3.cpp"
)
file(GLOB EXTENSION_SOURCES "src/core/extension/*.cpp")

add_library(environ STATIC ${ENVIRON_SOURCES})
add_library(sound STATIC ${SOUND_SOURCES})
add_library(visual STATIC ${VISUAL_SOURCES})
add_library(extension STATIC ${EXTENSION_SOURCES})

target_link_libraries(environ base)
target_link_libraries(visual base)
target_link_libraries(sound base)
target_link_libraries(extension base)

target_link_libraries(base tjs2 utils msg z archive bz2 zstd)

add_executable(kk4v src/main.cpp src/vita_backend.cpp src/vita_klog.cpp src/vita_movie.cpp)
target_link_libraries(kk4v -Wl,--start-group extension sound environ visual base tjs2 utils msg onig -Wl,--end-group
    -lavformat -lavcodec -lswscale -lswresample -lavutil -lmp3lame -lmpg123
    -lSDL2 -lopenal -lopusfile -lopus -lvorbisfile -lvorbis -logg
    -lfreetype -lpng -lturbojpeg -ljpeg -lwebp -lz -lm -lc
    -lSceDisplay_stub -lSceCtrl_stub -lSceAudio_stub -lSceAudioIn_stub
    -lSceRtc_stub -lSceGxm_stub -lSceSysmodule_stub -lSceMotion_stub
    -lSceTouch_stub -lSceFiber_stub -lScePower_stub -lSceCommonDialog_stub
    -lSceHid_stub -lSceAppUtil_stub -lScePgf_stub -lSceIme_stub -lSceAppMgr_stub
)

include("$ENV{VITASDK}/share/vita.cmake")
vita_create_self(kk4v.self kk4v)
vita_create_vpk(kk4v.vpk "KIRIKIRI2" kk4v.self
    VERSION 01.00
    NAME "KK4V - KiriKiri 4 Vita"
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.ttf default.ttf
)
