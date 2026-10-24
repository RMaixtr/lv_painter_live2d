cmake_minimum_required(VERSION 3.15)

project(lv_painter_live2d VERSION 0.1.0 LANGUAGES C)

option(LV_PAINTER_LIVE2D_BUILD_WIDGET "Build the LVGL 8 widget adapter" ON)
set(LV_PAINTER_LIVE2D_LVGL_TARGET "" CACHE STRING
    "LVGL CMake target used by the widget adapter (for example lvgl or lvgl::lvgl)")

add_library(pe_live pe_live.c)
add_library(lv_painter_live2d::pe_live ALIAS pe_live)

target_include_directories(pe_live PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/lv_painter_live2d>
)

set_target_properties(pe_live PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON
)

include(CheckLibraryExists)
check_library_exists(m sinf "" LV_PAINTER_LIVE2D_HAVE_LIBM)
if(LV_PAINTER_LIVE2D_HAVE_LIBM)
    target_link_libraries(pe_live PUBLIC m)
endif()

if(LV_PAINTER_LIVE2D_BUILD_WIDGET)
    add_library(lv_painter_live2d lv_live2d.c)
    add_library(lv_painter_live2d::lv_painter_live2d ALIAS lv_painter_live2d)

    target_include_directories(lv_painter_live2d PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/lv_painter_live2d>
    )
    set_target_properties(lv_painter_live2d PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON
    )
    target_link_libraries(lv_painter_live2d PUBLIC pe_live)

    set(LV_PAINTER_LIVE2D_RESOLVED_LVGL_TARGET "")
    if(LV_PAINTER_LIVE2D_LVGL_TARGET)
        if(NOT TARGET ${LV_PAINTER_LIVE2D_LVGL_TARGET})
            message(FATAL_ERROR
                "LV_PAINTER_LIVE2D_LVGL_TARGET=${LV_PAINTER_LIVE2D_LVGL_TARGET} does not exist")
        endif()
        set(LV_PAINTER_LIVE2D_RESOLVED_LVGL_TARGET ${LV_PAINTER_LIVE2D_LVGL_TARGET})
    elseif(TARGET lvgl::lvgl)
        set(LV_PAINTER_LIVE2D_RESOLVED_LVGL_TARGET lvgl::lvgl)
    elseif(TARGET lvgl)
        set(LV_PAINTER_LIVE2D_RESOLVED_LVGL_TARGET lvgl)
    else()
        message(FATAL_ERROR
            "No LVGL target found. Create an LVGL 8 target first, set "
            "LV_PAINTER_LIVE2D_LVGL_TARGET, or configure with "
            "-DLV_PAINTER_LIVE2D_BUILD_WIDGET=OFF.")
    endif()

    target_link_libraries(lv_painter_live2d PUBLIC ${LV_PAINTER_LIVE2D_RESOLVED_LVGL_TARGET})
    message(STATUS "lv_painter_live2d uses LVGL target: ${LV_PAINTER_LIVE2D_RESOLVED_LVGL_TARGET}")
endif()

include(GNUInstallDirs)

if(LV_PAINTER_LIVE2D_BUILD_WIDGET)
    install(TARGETS pe_live lv_painter_live2d
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES pe_live.h lv_live2d.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lv_painter_live2d
    )
else()
    install(TARGETS pe_live
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES pe_live.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lv_painter_live2d
    )
endif()

install(FILES LICENSE THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/lv_painter_live2d
)
