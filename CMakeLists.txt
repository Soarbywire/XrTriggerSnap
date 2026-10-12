cmake_minimum_required(VERSION 3.16)
project(XrTriggerSnap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(XR_APILAYER_trigger_snap SHARED src/layer.cpp)
target_include_directories(XR_APILAYER_trigger_snap PRIVATE external)
set_target_properties(XR_APILAYER_trigger_snap PROPERTIES PREFIX "")

if(MSVC)
    # Static CRT so the DLL needs no Visual C++ redistributable.
    set_property(TARGET XR_APILAYER_trigger_snap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Copy the built DLL into dist/ next to the manifest and ini.
add_custom_command(TARGET XR_APILAYER_trigger_snap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:XR_APILAYER_trigger_snap> ${CMAKE_SOURCE_DIR}/dist/)

# Tray app (Windows only).
if(WIN32)
    enable_language(RC)
    add_executable(XrTriggerSnap WIN32 tray/tray.cpp tray/updater.cpp tray/tray.rc)
    target_compile_definitions(XrTriggerSnap PRIVATE UNICODE _UNICODE)
    target_link_libraries(XrTriggerSnap PRIVATE comctl32 shell32 advapi32 user32 gdi32 winhttp bcrypt)
    if(MSVC)
        set_property(TARGET XrTriggerSnap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # The manifest is embedded via tray.rc.
        target_link_options(XrTriggerSnap PRIVATE /MANIFEST:NO)
    else()
        target_compile_options(XrTriggerSnap PRIVATE -municode)
        target_link_options(XrTriggerSnap PRIVATE -municode -static)
    endif()
    add_custom_command(TARGET XrTriggerSnap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:XrTriggerSnap> ${CMAKE_SOURCE_DIR}/dist/)
endif()
