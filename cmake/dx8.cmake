# Map DX8 library names to DX9 libraries for linking.
# The DX8 SDK is no longer fetched; the DX9 SDK (dx9.cmake) provides all
# necessary headers and libraries. These interface targets allow existing
# CMakeLists that reference DX8 library names to link correctly.
if(NOT TARGET d3d8)
    add_library(d3d8 INTERFACE)
    target_link_libraries(d3d8 INTERFACE d3d9)
endif()

if(NOT TARGET d3dx8)
    add_library(d3dx8 INTERFACE)
    target_link_libraries(d3dx8 INTERFACE d3dx9)
endif()

if(NOT TARGET d3d8lib)
    add_library(d3d8lib INTERFACE)
    target_link_libraries(d3d8lib INTERFACE d3d9lib)
endif()
