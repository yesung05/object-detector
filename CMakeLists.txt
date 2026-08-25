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
set(FFMPEG_FOUND TRUE)
foreach(component avformat avcodec avutil swscale avdevice)
    string(TOUPPER "${component}" component_upper)
    find_library(FFMPEG_${component_upper}_LIBRARY NAMES ${component}
        HINTS "${FFMPEG_ROOT}"
        PATH_SUFFIXES lib lib64)
    if(NOT FFMPEG_${component_upper}_LIBRARY)
        set(FFMPEG_FOUND FALSE)
    else()
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_${component_upper}_LIBRARY}")
    endif()
endforeach()

# ORT와 FFmpeg가 모두 있을 때만 메인 실행 파일을 빌드합니다.
# 없으면 test_core(외부 의존 없음)만 빌드됩니다.
if(ORT_INCLUDE_DIR AND ORT_LIBRARY AND FFMPEG_INCLUDE_DIR AND FFMPEG_FOUND)
    set(runtime_dlls)
    if(WIN32)
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
        foreach(runtime_dll IN LISTS runtime_dlls)
            add_custom_command(TARGET yolo11-person POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${runtime_dll}" "$<TARGET_FILE_DIR:yolo11-person>")
        endforeach()
    else()
        target_compile_options(yolo11-person PRIVATE
            -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(NOT WIN32)
            target_link_libraries(yolo11-person PRIVATE m)
        endif()
    endif()
else()
    message(STATUS "yolo11-person 빌드 스킵: ORT_ROOT 또는 FFMPEG_ROOT 미설정")
    message(STATUS "  단위 테스트(test_core)는 외부 의존 없이 빌드됩니다.")
endif()

# ── 단위 테스트 (ORT/FFmpeg 의존 없음) ───────────────────────────────────
enable_testing()

add_executable(test_core
    tests/test_core.c
    src/postprocess.c
    src/draw.c
    src/tracker.c
    src/platform.c)
target_include_directories(test_core PRIVATE include tests)
if(MSVC)
    target_compile_options(test_core PRIVATE /O1 /W4)
    target_compile_definitions(test_core PRIVATE
        WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
else()
    target_compile_options(test_core PRIVATE
        -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_link_libraries(test_core PRIVATE m)
endif()
add_test(NAME core_unit COMMAND test_core)
set_tests_properties(core_unit PROPERTIES LABELS "unit")

# ── 통합 테스트 (ORT 필요, FFmpeg 불필요) ────────────────────────────────
if(ORT_INCLUDE_DIR AND ORT_LIBRARY)
    add_executable(test_detector
        tests/test_detector.c
        src/postprocess.c
        src/draw.c
        src/tracker.c
        src/platform.c
        src/detector_ort.c)
    target_include_directories(test_detector PRIVATE
        include "${ORT_INCLUDE_DIR}" tests)
    target_link_libraries(test_detector PRIVATE "${ORT_LIBRARY}")
    if(MSVC)
        target_compile_options(test_detector PRIVATE /O1 /W4)
        target_compile_definitions(test_detector PRIVATE
            ORT_DLL_IMPORT WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
        foreach(runtime_dll IN LISTS runtime_dlls)
            add_custom_command(TARGET test_detector POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${runtime_dll}" "$<TARGET_FILE_DIR:test_detector>")
        endforeach()
    else()
        target_compile_options(test_detector PRIVATE
            -O1 -g -Wall -Wextra -Wpedantic)
        target_link_libraries(test_detector PRIVATE m)
    endif()
    add_test(NAME detector_integration
        COMMAND test_detector "${CMAKE_SOURCE_DIR}/yolo11n-416.onnx")
    set_tests_properties(detector_integration PROPERTIES
        TIMEOUT 60
        LABELS "integration")
endif()
