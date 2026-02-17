# DirectX 9 SDK Configuration
# Ronin 19/10/2025 Added DX9 SDK path for compatibility layer

# Try to find DirectX 9 SDK
set(DX9_SDK_PATH "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)")

if(EXISTS "${DX9_SDK_PATH}")
    message(STATUS "Found DirectX 9 SDK at: ${DX9_SDK_PATH}")
    
    # Ronin: Add include directories GLOBALLY with BEFORE to ensure DX9 headers take absolute precedence
    include_directories(BEFORE SYSTEM "${DX9_SDK_PATH}/Include")
    link_directories("${DX9_SDK_PATH}/Lib/x86")
    
    # Create interface library for DX9
    add_library(dx9 INTERFACE)
    target_include_directories(dx9 SYSTEM INTERFACE BEFORE "${DX9_SDK_PATH}/Include")
    target_link_directories(dx9 INTERFACE "${DX9_SDK_PATH}/Lib/x86")
    target_link_libraries(dx9 INTERFACE d3d9 d3dx9)
    
else()
    message(WARNING "DirectX 9 SDK not found at: ${DX9_SDK_PATH}")
    message(WARNING "Please install DirectX SDK June 2010 or update DX9_SDK_PATH in cmake/dx9.cmake")
endif()
