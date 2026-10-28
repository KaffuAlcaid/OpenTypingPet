cmake_minimum_required(VERSION 3.24)
project(OpenTypingPet VERSION 0.1.0 LANGUAGES C CXX RC)
if(NOT WIN32)
    message(FATAL_ERROR "OpenTypingPet requires Windows.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(miniz
    URL https://codeload.github.com/richgel999/miniz/tar.gz/293d4db1b7d0ffee9756d035b9ac6f7431ef8492
    URL_HASH SHA256=27d99216e9127d1678085639f3059f6907a02c044e8a38392a39f96163ddbbf0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniz)
add_executable(OpenTypingPet WIN32
    src/main.cpp src/appController.cpp src/presetLoader.cpp src/petEngine.cpp
    src/inputSource.cpp src/appSettings.cpp src/appResources.rc)
target_compile_definitions(OpenTypingPet PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(OpenTypingPet PRIVATE /W4 /utf-8 $<$<CONFIG:Release>:/O1> $<$<CONFIG:Release>:/GL>)
    target_link_options(OpenTypingPet PRIVATE $<$<CONFIG:Release>:/LTCG> /OPT:REF /OPT:ICF "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
endif()
target_link_libraries(OpenTypingPet PRIVATE miniz user32 gdi32 gdiplus comctl32 shell32 shlwapi
    ole32 uuid advapi32 bcrypt dwmapi uxtheme wtsapi32)
set_target_properties(OpenTypingPet PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
install(TARGETS OpenTypingPet RUNTIME DESTINATION .)
install(FILES LICENSE DESTINATION licenses RENAME OpenTypingPet.txt)
install(FILES "${miniz_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME miniz.txt)
install(FILES assets/LUCIDE-LICENSE DESTINATION licenses RENAME Lucide.txt)
