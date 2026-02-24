add_executable(z_generals WIN32)

# Use a binary name that doesn't conflict with original game.
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME generalszh)
else()
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME generalszh)
endif()

target_link_libraries(z_generals PRIVATE
    binkstub
    comctl32
    core_debug
    core_profile
    d3d9
    d3dx9
    dinput8
    dxguid
    imm32
    milesstub
    vfw32
    winmm
    z_gameengine
    z_gameenginedevice
    zi_always
)

# TODO Originally referred to build host and user, replace with git info perhaps?
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h
"#pragma once

#define VERSION_LOCALBUILDNUM 0
#define VERSION_BUILDUSER \"\"
#define VERSION_BUILDLOC \"\"
"
)

# Based on original binary values for these variables.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_BUILDNUM 601
"
)

if(MSVC)
    target_link_options(z_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(z_generals PRIVATE
    WinMain.cpp
    WinMain.h
)

# RC files optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(z_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(z_generals PRIVATE
        RTS.RC
    )
endif()

if(MSVC)
    # Ronin @build 02/11/2025 AUTOMATIC deployment to Zero Hour folder
    # This copies exe + pdb to Zero Hour installation after every build
    set(ZEROHOUR_INSTALL_PATH "C:/Program Files (x86)/Steam/steamapps/common/Command & Conquer Generals - Zero Hour")
    
    # Ronin @build 18/02/2026 Path to the compiled instancing vertex shader
    set(SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shaders")

    add_custom_command(TARGET z_generals POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Deploying to Zero Hour installation..."
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:z_generals>
   "${ZEROHOUR_INSTALL_PATH}/generalszh.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
   $<TARGET_FILE_DIR:z_generals>/generalszh.pdb
        "${ZEROHOUR_INSTALL_PATH}/generalszh.pdb"
        # Ronin @build 18/02/2026 Deploy instancing vertex shader to shaders subfolder
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${ZEROHOUR_INSTALL_PATH}/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHADER_SOURCE_DIR}/RigidInstance.vso"
            "${ZEROHOUR_INSTALL_PATH}/shaders/RigidInstance.vso"
        COMMENT "Auto-deploying to Zero Hour installation folder"

    )
    
    # Ronin @build 02/11/2025 Set debugging working directory for Visual Studio
    # This makes F5 debugging run from the Zero Hour installation folder
    set_target_properties(z_generals PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${ZEROHOUR_INSTALL_PATH}"
 VS_DEBUGGER_COMMAND_ARGUMENTS "-win -quickstart -xres 1280 -yres 720"
    )
endif()
