cmake_minimum_required(VERSION 3.15)
project(ArinCaptureSBS VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# When using Ninja with MSVC, the environment may not provide INCLUDE paths
# (e.g. building from a plain PowerShell), which can break resource compilation.
# Add Windows SDK include dirs explicitly for the RC compiler.
if (WIN32 AND CMAKE_RC_COMPILER)
    # Example: C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/rc.exe
    get_filename_component(_ac_rc_dir "${CMAKE_RC_COMPILER}" DIRECTORY)
    get_filename_component(_ac_rc_ver_dir "${_ac_rc_dir}" DIRECTORY)
    get_filename_component(_ac_rc_bin_root "${_ac_rc_ver_dir}" DIRECTORY)
    get_filename_component(_ac_kits_root "${_ac_rc_bin_root}" DIRECTORY)
    get_filename_component(_ac_kits_ver "${_ac_rc_ver_dir}" NAME)
    set(_ac_sdk_include_root "${_ac_kits_root}/Include/${_ac_kits_ver}")

    if (EXISTS "${_ac_sdk_include_root}/um/winres.h")
        set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${_ac_sdk_include_root}/um\" /I\"${_ac_sdk_include_root}/shared\"")
    endif()
endif()

add_executable(ArinCaptureSBS
    src/main.cpp
    src/Settings.cpp
    src/Settings.h
    src/CaptureDXGI.cpp
    src/CaptureDXGI.h
    src/CaptureWGC.cpp
    src/CaptureWGC.h
    src/Monitors.cpp
    src/Monitors.h
    src/DxgiCrop.cpp
    src/DxgiCrop.h
    src/WindowTargeting.cpp
    src/WindowTargeting.h
    src/TrayIcon.cpp
    src/TrayIcon.h
    src/Renderer.cpp
    src/Renderer.h
    src/DepthDialog.cpp
    src/DepthDialog.h
    src/Log.cpp
    src/Log.h
    src/3PassShader.cpp
    src/3PassShader.h
    res/resource.rc
    res/resource.cpp # Dummy file to ensure CMake compiles the resource
)

set_target_properties(ArinCaptureSBS PROPERTIES WIN32_EXECUTABLE TRUE)

# MSVC can fail with LNK1168 if the previous output EXE is still held open
# (e.g., crash handler, AV scan, or a lingering running instance). Removing
# the existing output before linking avoids the overwrite path.
if (WIN32)
    add_custom_command(TARGET ArinCaptureSBS PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE:ArinCaptureSBS>"
        VERBATIM
    )
endif()

target_link_libraries(ArinCaptureSBS
    d3d11
    d3dcompiler
    dxgi
    dxguid
    windowsapp
    user32
    gdi32
    shell32
    comdlg32
    ole32
    oleaut32
    uuid
    winmm
)

# ---- Build ID embedding ----
# Embed a build identifier into the executable so Debug/Release binaries can be
# unambiguously distinguished in logs and when comparing behavior.
find_package(Git QUIET)
set(AC_GIT_SHA "nogit")
if (GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE AC_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

target_compile_definitions(ArinCaptureSBS PRIVATE
    "AC_BUILD_CONFIG=\"$<CONFIG>\""
    "AC_GIT_SHA=\"${AC_GIT_SHA}\""
    "AC_PROJECT_VERSION=\"${PROJECT_VERSION}\""
)

# ---- Packaging / Install (for alpha builds) ----
install(TARGETS ArinCaptureSBS
    RUNTIME DESTINATION .
)

install(FILES
    README.md
    DESTINATION .
)

# ZIP packaging via CPack (runs after `cmake --install` or via `cpack`).
set(CPACK_PACKAGE_NAME "ArinCaptureSBS")
set(CPACK_PACKAGE_VENDOR "Arin")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
