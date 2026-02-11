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
    dbghelplib
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

target_link_options(z_generals PRIVATE "/NODEFAULTLIB:libci.lib")

target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(z_generals PRIVATE
    WinMain.cpp
    WinMain.h
)

if(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(z_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(z_generals PRIVATE
        RTS.RC
    )
    
    # Ronin @build 02/11/2025 Add post-build DLL copy commands
    # Copy binkw32.dll and mss32.dll to exe directory
    add_custom_command(TARGET z_generals POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:binkstub>
            $<TARGET_FILE_DIR:z_generals>
        COMMENT "Copying binkw32.dll to output directory"
    )
    
    add_custom_command(TARGET z_generals POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:milesstub>
            $<TARGET_FILE_DIR:z_generals>
        COMMENT "Copying mss32.dll to output directory"
    )

    # Ronin @build 02/11/2025 AUTOMATIC deployment to Zero Hour folder
    # This copies exe + pdb to Zero Hour installation after every build
    set(ZEROHOUR_INSTALL_PATH "C:/Program Files (x86)/Steam/steamapps/common/Command & Conquer Generals - Zero Hour")
    
    add_custom_command(TARGET z_generals POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Deploying to Zero Hour installation..."
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:z_generals>
   "${ZEROHOUR_INSTALL_PATH}/generalszh.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
   $<TARGET_FILE_DIR:z_generals>/generalszh.pdb
        "${ZEROHOUR_INSTALL_PATH}/generalszh.pdb"
        COMMENT "Auto-deploying to Zero Hour installation folder"
    )
    
    # Ronin @build 02/11/2025 Set debugging working directory for Visual Studio
    # This makes F5 debugging run from the Zero Hour installation folder
    set_target_properties(z_generals PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${ZEROHOUR_INSTALL_PATH}"
 VS_DEBUGGER_COMMAND_ARGUMENTS "-win -quickstart -xres 1280 -yres 720"
    )
endif()
