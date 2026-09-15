cmake_minimum_required(VERSION 3.21)

include(cmake/LpmMetadata.cmake)
project(${LILYGO_UI_CMAKE_PROJECT_NAME} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
find_package(LilyGoUI CONFIG REQUIRED)
find_package(Threads REQUIRED)

configure_file(
    cmake/app_identity.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/app_identity.h
    @ONLY)

add_library(lilygo_ui_radio_core STATIC
    src/pages/radio/radio_model.cpp
    src/services/stream_player.cpp
    src/services/radio_browser_client.cpp
    src/services/artwork_cache.cpp)
target_link_libraries(lilygo_ui_radio_core PUBLIC Threads::Threads)
target_include_directories(lilygo_ui_radio_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_radio_core PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_radio_core PUBLIC cxx_std_17)

add_library(lilygo_ui_radio_ui STATIC
    src/app.cpp
    src/components/components.cpp
    src/components/host_text_input.cpp
    src/pages/radio/radio_view.cpp
    src/pages/radio/radio_view_model.cpp)
target_include_directories(lilygo_ui_radio_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_radio_ui PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_radio_ui PUBLIC
    lilygo_ui_radio_core
    LilyGoUI::Runtime)

add_executable(lilygo_ui_radio src/main.cpp)
target_compile_options(lilygo_ui_radio PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_radio PRIVATE lilygo_ui_radio_ui)
set_target_properties(lilygo_ui_radio PROPERTIES
    OUTPUT_NAME "${LILYGO_UI_EXECUTABLE_NAME}")

configure_file(data/launcher.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json @ONLY)
configure_file(data/application.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop @ONLY)
configure_file(data/application.metainfo.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml @ONLY)

install(TARGETS lilygo_ui_radio
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/launcher/apps
    COMPONENT app)
lilygo_ui_install_launcher_icon(
    "${LILYGO_UI_APP_ID}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${LILYGO_UI_APP_ICON}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    COMPONENT app)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}"
    app)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(radio-artwork-tests tests/test_artwork.cpp)
    target_compile_options(radio-artwork-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(radio-artwork-tests PRIVATE lilygo_ui_radio_core)
    add_test(NAME radio-artwork COMMAND radio-artwork-tests)
    add_executable(radio-core-tests tests/test_core.cpp)
    target_compile_options(radio-core-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(radio-core-tests PRIVATE
        lilygo_ui_radio_core)
    add_test(NAME radio-core COMMAND radio-core-tests)

    add_executable(radio-browser-tests tests/test_radio_browser.cpp)
    target_compile_options(radio-browser-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(radio-browser-tests PRIVATE lilygo_ui_radio_core)
    add_test(NAME radio-browser COMMAND radio-browser-tests)

    add_executable(radio-view-model-tests tests/test_view_model.cpp)
    target_compile_options(radio-view-model-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(radio-view-model-tests PRIVATE
        lilygo_ui_radio_ui)
    add_test(NAME radio-view-model COMMAND radio-view-model-tests)

    if(CM0_SIMULATOR)
        add_executable(radio-host-input-tests tests/test_host_input.cpp)
        target_compile_options(radio-host-input-tests PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)
        target_link_libraries(radio-host-input-tests PRIVATE lilygo_ui_radio_ui)
        if(SDL2_VERSION VERSION_GREATER_EQUAL "2.32.50")
            find_path(_radio_sdl3_include SDL3/SDL_events.h REQUIRED)
            find_library(_radio_sdl3_library NAMES SDL3 REQUIRED)
            target_sources(radio-host-input-tests PRIVATE tests/sdl3_text_event.cpp)
            target_include_directories(radio-host-input-tests PRIVATE ${_radio_sdl3_include})
            target_compile_definitions(radio-host-input-tests PRIVATE
                RADIO_TEST_SDL3_LIBRARY="${_radio_sdl3_library}")
            target_link_libraries(radio-host-input-tests PRIVATE ${CMAKE_DL_LIBS})
        endif()
        add_test(NAME radio-host-input COMMAND radio-host-input-tests)
    endif()

    add_executable(radio-render-ui tests/render_ui.cpp)
    target_compile_options(radio-render-ui PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(radio-render-ui PRIVATE
        lilygo_ui_radio_ui)
    foreach(viewport IN ITEMS portrait landscape mobile compact)
        if(viewport STREQUAL "portrait")
            set(_width 568)
            set(_height 1232)
        elseif(viewport STREQUAL "landscape")
            set(_width 1232)
            set(_height 568)
        elseif(viewport STREQUAL "mobile")
            set(_width 390)
            set(_height 844)
        else()
            set(_width 320)
            set(_height 568)
        endif()
        add_test(NAME radio-render-${viewport} COMMAND radio-render-ui
            ${_width} ${_height}
            ${CMAKE_CURRENT_BINARY_DIR}/radio-${viewport}.ppm)
        add_test(NAME radio-render-${viewport}-dialog COMMAND radio-render-ui
            ${_width} ${_height}
            ${CMAKE_CURRENT_BINARY_DIR}/radio-${viewport}-dialog.ppm
            dialog)
        foreach(scenario IN ITEMS search empty error stations favorites settings sleep artwork)
            add_test(NAME radio-render-${viewport}-${scenario} COMMAND radio-render-ui
                ${_width} ${_height}
                ${CMAKE_CURRENT_BINARY_DIR}/radio-${viewport}-${scenario}.ppm
                ${scenario})
        endforeach()
    endforeach()
endif()

# New applications have no legacy CM0 package to replace. Add Debian migration
# fields only when this project supersedes an actually published package.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${LILYGO_UI_VERSION}")
if(LILYGO_UI_VERSION MATCHES
   "^([0-9]+:)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_4}")
else()
    set(CPACK_PACKAGE_VERSION_MAJOR 0)
    set(CPACK_PACKAGE_VERSION_MINOR 0)
    set(CPACK_PACKAGE_VERSION_PATCH 0)
endif()
set(CPACK_PACKAGE_CONTACT "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_VENDOR "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${LILYGO_UI_APP_HOMEPAGE}")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_SET_DESTDIR ON)
set(CPACK_COMPONENTS_ALL app)
set(CPACK_DEBIAN_APP_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_DEBIAN_APP_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_APP_PACKAGE_DEPENDS
    "libc6, libstdc++6, libdrm2, libfreetype6, curl, ffmpeg, alsa-utils, lilygo-ui-appkit-dev (>= ${LILYGO_UI_MIN_APPKIT_VERSION})")
set(CPACK_DEBIAN_APP_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        "${LILYGO_UI_PACKAGE_ARCHITECTURE}")
endif()
include(CPack)
