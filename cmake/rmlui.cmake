# Ronin @feature 28/10/2026 DX9: RmlUi for the modern control bar (docs/UI_Work.md). vcpkg's copy when the build has one,
# else FreeType and RmlUi fetched at pinned tags (no submodules: FreeType's dlg is debug logging) and built static.

find_package(RmlUi CONFIG QUIET)

if(NOT RmlUi_FOUND)
    include(FetchContent)

    FetchContent_Declare(
        freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG        VER-2-14-1
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
        EXCLUDE_FROM_ALL
        SYSTEM
    )

    FetchContent_Declare(
        rmlui
        GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
        GIT_TAG        6.3
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
        EXCLUDE_FROM_ALL
        SYSTEM
    )

    # A function, so these settings stay with the two libraries.
    function(rts_fetch_rmlui)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TESTING OFF)
        set(FT_DISABLE_ZLIB ON)
        set(FT_DISABLE_BZIP2 ON)
        set(FT_DISABLE_PNG ON)
        set(FT_DISABLE_HARFBUZZ ON)
        set(FT_DISABLE_BROTLI ON)
        FetchContent_MakeAvailable(freetype)

        # RmlUi only asks that a Freetype::Freetype target exist (its CMake/Dependencies.cmake).
        add_library(Freetype::Freetype ALIAS freetype)

        set(RMLUI_SAMPLES OFF)
        FetchContent_MakeAvailable(rmlui)
    endfunction()

    rts_fetch_rmlui()
endif()
