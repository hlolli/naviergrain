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
find_program(NAVIERGRAIN_BUN bun REQUIRED)
file(GLOB_RECURSE _ng_ui_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../ui/*")
set(_ng_ui_header "${CMAKE_CURRENT_BINARY_DIR}/generated/naviergrain_ui.h")
add_custom_command(OUTPUT "${_ng_ui_header}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/build_ui.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/ui" --header "${_ng_ui_header}" --bun "${NAVIERGRAIN_BUN}"
  DEPENDS ${_ng_ui_files} "${CMAKE_CURRENT_LIST_DIR}/../tokens.css"
    "${CMAKE_CURRENT_LIST_DIR}/../LICENSE" "${CMAKE_CURRENT_LIST_DIR}/../COPYRIGHT"
    "${CMAKE_CURRENT_LIST_DIR}/../schema/naviergrain-v1.json"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/build_ui.py"
  VERBATIM)
add_library(naviergrain_live STATIC src/naviergrain_live.c)
target_link_libraries(naviergrain_live PUBLIC naviergrain_core)
if(NAVIERGRAIN_NATIVE_WORKER)
  target_compile_definitions(naviergrain_live PRIVATE NG_LIVE_NATIVE)
  target_link_libraries(naviergrain_live PUBLIC naviergrain_native_gpu)
endif()
add_executable(naviergrain_app MACOSX_BUNDLE desktop/main.cpp desktop/audio.cpp desktop/miniaudio.c "${_ng_ui_header}")
target_compile_features(naviergrain_app PRIVATE cxx_std_17)
if(NAVIERGRAIN_METAL)
  target_compile_definitions(naviergrain_app PRIVATE NG_DESKTOP_GPU_NAME="Metal")
elseif(NAVIERGRAIN_CUDA)
  target_compile_definitions(naviergrain_app PRIVATE NG_DESKTOP_GPU_NAME="CUDA")
else()
  target_compile_definitions(naviergrain_app PRIVATE NG_DESKTOP_GPU_NAME="CPU")
endif()
target_include_directories(naviergrain_app PRIVATE "${miniaudio_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(naviergrain_app PRIVATE webview::core naviergrain_live Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(naviergrain_app PROPERTIES OUTPUT_NAME naviergrain
  MACOSX_BUNDLE_BUNDLE_NAME naviergrain MACOSX_BUNDLE_GUI_IDENTIFIER org.naviergrain.instrument
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "Copyright (C) 2026 Hlöðver Sigurðsson")
set(_ng_licenses "${CMAKE_CURRENT_BINARY_DIR}/generated/licenses")
file(MAKE_DIRECTORY "${_ng_licenses}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../LICENSE" "${_ng_licenses}/naviergrain-GPL-3.0.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../COPYRIGHT" "${_ng_licenses}/naviergrain-copyright.txt" COPYONLY)
configure_file("${webview_SOURCE_DIR}/LICENSE" "${_ng_licenses}/webview.txt" COPYONLY)
configure_file("${miniaudio_SOURCE_DIR}/LICENSE" "${_ng_licenses}/miniaudio.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../ui/fonts/ibmplexsans-OFL.txt" "${_ng_licenses}/ibm-plex-sans.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../ui/fonts/spacegrotesk-OFL.txt" "${_ng_licenses}/space-grotesk.txt" COPYONLY)
file(GLOB _ng_license_files "${_ng_licenses}/*.txt")
target_sources(naviergrain_app PRIVATE ${_ng_license_files})
set_source_files_properties(${_ng_license_files} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
if(APPLE)
  set_target_properties(naviergrain_app PROPERTIES LINKER_LANGUAGE OBJCXX)
  target_sources(naviergrain_app PRIVATE desktop/platform.mm)
  set_source_files_properties(desktop/platform.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(naviergrain_app PRIVATE "-framework AppKit")
else()
  target_sources(naviergrain_app PRIVATE desktop/platform.cpp)
  if(WIN32)
    target_link_libraries(naviergrain_app PRIVATE shell32)
  endif()
endif()
