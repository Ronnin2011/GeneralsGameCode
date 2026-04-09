FetchContent_Declare(
    dx9
    GIT_REPOSITORY https://github.com/stephanmeesters/min-dx9-sdk.git
    GIT_TAG        d7c1c587f3bbab03900e8a6367669bb56e30e8b3
)

FetchContent_MakeAvailable(dx9)

# Add DX9 SDK include and lib directories globally so that targets linking
# d3d9 or d3dx9 directly can find the headers and libraries.
include_directories(BEFORE SYSTEM "${dx9_SOURCE_DIR}")
link_directories("${dx9_SOURCE_DIR}")
