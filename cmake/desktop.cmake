enable_language(CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()
include(FetchContent)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(webview
  URL https://codeload.github.com/webview/webview/tar.gz/782c12ccc3e9358c14a42b41baddbf50bfc28614)
FetchContent_Declare(miniaudio
  URL https://codeload.github.com/mackron/miniaudio/tar.gz/f40cf03f80cdb7e741d43e53b7e706e8c1394bcf
  SOURCE_SUBDIR header-only)
# Only use miniaudio's pinned header, without adding its project targets.
FetchContent_MakeAvailable(webview miniaudio)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Threads REQUIRED)
find_program(FLUIDGRAIN_BUN bun REQUIRED)
file(GLOB_RECURSE _fg_ui_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../ui/*")
set(_fg_ui_header "${CMAKE_CURRENT_BINARY_DIR}/generated/fluidgrain_ui.h")
add_custom_command(OUTPUT "${_fg_ui_header}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/build_ui.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/ui" --header "${_fg_ui_header}" --bun "${FLUIDGRAIN_BUN}"
  DEPENDS ${_fg_ui_files} "${CMAKE_CURRENT_LIST_DIR}/../tokens.css"
    "${CMAKE_CURRENT_LIST_DIR}/../LICENSE" "${CMAKE_CURRENT_LIST_DIR}/../COPYRIGHT"
    "${CMAKE_CURRENT_LIST_DIR}/../schema/fluidgrain-v1.json"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/build_ui.py"
  VERBATIM)
add_library(fluidgrain_live STATIC src/fluidgrain_live.c)
target_link_libraries(fluidgrain_live PUBLIC fluidgrain_core)
if(FLUIDGRAIN_NATIVE_WORKER)
  target_compile_definitions(fluidgrain_live PRIVATE FG_LIVE_NATIVE)
  target_link_libraries(fluidgrain_live PUBLIC fluidgrain_native_gpu)
endif()
add_executable(fluidgrain_app MACOSX_BUNDLE desktop/main.cpp desktop/audio.cpp desktop/miniaudio.c "${_fg_ui_header}")
target_compile_features(fluidgrain_app PRIVATE cxx_std_17)
if(FLUIDGRAIN_METAL)
  target_compile_definitions(fluidgrain_app PRIVATE FG_DESKTOP_GPU_NAME="Metal")
elseif(FLUIDGRAIN_CUDA)
  target_compile_definitions(fluidgrain_app PRIVATE FG_DESKTOP_GPU_NAME="CUDA")
else()
  target_compile_definitions(fluidgrain_app PRIVATE FG_DESKTOP_GPU_NAME="CPU")
endif()
target_include_directories(fluidgrain_app PRIVATE "${miniaudio_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(fluidgrain_app PRIVATE webview::core fluidgrain_live Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(fluidgrain_app PROPERTIES OUTPUT_NAME naviergrain
  MACOSX_BUNDLE_BUNDLE_NAME naviergrain MACOSX_BUNDLE_GUI_IDENTIFIER org.naviergrain.instrument
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "Copyright (C) 2026 Hlöðver Sigurðsson")
set(_fg_licenses "${CMAKE_CURRENT_BINARY_DIR}/generated/licenses")
file(MAKE_DIRECTORY "${_fg_licenses}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../LICENSE" "${_fg_licenses}/naviergrain-GPL-3.0.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../COPYRIGHT" "${_fg_licenses}/naviergrain-copyright.txt" COPYONLY)
configure_file("${webview_SOURCE_DIR}/LICENSE" "${_fg_licenses}/webview.txt" COPYONLY)
configure_file("${miniaudio_SOURCE_DIR}/LICENSE" "${_fg_licenses}/miniaudio.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../ui/fonts/ibmplexsans-OFL.txt" "${_fg_licenses}/ibm-plex-sans.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../ui/fonts/spacegrotesk-OFL.txt" "${_fg_licenses}/space-grotesk.txt" COPYONLY)
file(GLOB _fg_license_files "${_fg_licenses}/*.txt")
target_sources(fluidgrain_app PRIVATE ${_fg_license_files})
set_source_files_properties(${_fg_license_files} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
if(APPLE)
  set_target_properties(fluidgrain_app PROPERTIES LINKER_LANGUAGE OBJCXX)
  target_sources(fluidgrain_app PRIVATE desktop/platform.mm)
  set_source_files_properties(desktop/platform.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(fluidgrain_app PRIVATE "-framework AppKit")
else()
  target_sources(fluidgrain_app PRIVATE desktop/platform.cpp)
  if(WIN32)
    target_link_libraries(fluidgrain_app PRIVATE shell32)
  endif()
endif()
