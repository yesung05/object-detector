cmake_minimum_required(VERSION 3.20)
project(yolo11_person C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(ORT_ROOT "" CACHE PATH "ONNX Runtime or Microsoft.ML.OnnxRuntime.DirectML package root")
set(DIRECTML_ROOT "" CACHE PATH "Microsoft.AI.DirectML package root (optional DLL copy)")
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg development package root")

find_path(ORT_INCLUDE_DIR onnxruntime_c_api.h
    HINTS "${ORT_ROOT}"
    PATH_SUFFIXES include build/native/include)
find_library(ORT_LIBRARY NAMES onnxruntime
    HINTS "${ORT_ROOT}"
    PATH_SUFFIXES lib lib64 runtimes/win-x64/native runtimes/win-x86/native)

find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
    HINTS "${FFMPEG_ROOT}"
    PATH_SUFFIXES include)
foreach(component avformat avcodec avutil swscale avdevice)
    string(TOUPPER "${component}" component_upper)
    find_library(FFMPEG_${component_upper}_LIBRARY NAMES ${component}
        HINTS "${FFMPEG_ROOT}"
        PATH_SUFFIXES lib lib64)
    if(NOT FFMPEG_${component_upper}_LIBRARY)
        message(FATAL_ERROR "FFmpeg library ${component} not found; set FFMPEG_ROOT")
    endif()
    list(APPEND FFMPEG_LIBRARIES "${FFMPEG_${component_upper}_LIBRARY}")
endforeach()

if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
    message(FATAL_ERROR "ONNX Runtime not found; set ORT_ROOT")
endif()
if(NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "FFmpeg headers not found; set FFMPEG_ROOT")
endif()

add_executable(yolo11-person
    src/main.c
    src/postprocess.c
    src/draw.c
    src/detector_ort.c
    src/media_ffmpeg.c
    src/tracker.c
    src/platform.c)
target_include_directories(yolo11-person PRIVATE
    include "${ORT_INCLUDE_DIR}" "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(yolo11-person PRIVATE "${ORT_LIBRARY}" ${FFMPEG_LIBRARIES})

if(MSVC)
    target_compile_definitions(yolo11-person PRIVATE
        ORT_DLL_IMPORT WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
    target_compile_options(yolo11-person PRIVATE /O2 /W4 /arch:AVX2)
else()
    target_compile_options(yolo11-person PRIVATE
        -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(NOT WIN32)
        target_link_libraries(yolo11-person PRIVATE m)
    endif()
endif()

if(WIN32)
    set(runtime_dlls)
    file(GLOB runtime_dlls CONFIGURE_DEPENDS
        "${ORT_ROOT}/runtimes/win-x64/native/*.dll"
        "${ORT_ROOT}/runtimes/win-x86/native/*.dll"
        "${ORT_ROOT}/lib/*.dll"
        "${FFMPEG_ROOT}/bin/*.dll")
    if(DIRECTML_ROOT)
        file(GLOB directml_runtime_dll CONFIGURE_DEPENDS
            "${DIRECTML_ROOT}/bin/x64-win/DirectML.dll"
            "${DIRECTML_ROOT}/bin/x86-win/DirectML.dll")
        list(APPEND runtime_dlls ${directml_runtime_dll})
    endif()
    foreach(runtime_dll IN LISTS runtime_dlls)
        add_custom_command(TARGET yolo11-person POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}" "$<TARGET_FILE_DIR:yolo11-person>")
    endforeach()
endif()
