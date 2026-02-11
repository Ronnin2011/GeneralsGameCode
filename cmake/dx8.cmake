# Ronin 19/10/2025 Disabled DX8 SDK fetch - using real DX9 SDK instead via dx9.cmake
# The min-dx8-sdk contains mixed DX8/DX9 headers causing type conflicts with our typedef layer

# FetchContent_Declare(
#     dx8
#     GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
#     GIT_TAG        20d31185872e1304e0573f7f4885ae11e50670d3
# )
# 
# FetchContent_MakeAvailable(dx8)

# Ronin 19/01/2026 Map DX8 library names to DX9 libraries for linking
# Create interface targets that link to the actual DX9 libraries
add_library(d3d8 INTERFACE)
add_library(d3dx8 INTERFACE)

# Link to DX9 libraries from the DirectX SDK
target_link_libraries(d3d8 INTERFACE d3d9)
target_link_libraries(d3dx8 INTERFACE d3dx9)
