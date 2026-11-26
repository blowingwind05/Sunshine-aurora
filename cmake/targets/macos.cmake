# macos specific target definitions

option(AURORA_MACOS_PRIVACY_PREVIEW "Enable Aurora's experimental macOS privacy curtain" OFF)
if(AURORA_MACOS_PRIVACY_PREVIEW)
    if(NOT AURORA_SUNSHINE_HOST)
        message(FATAL_ERROR "The Aurora macOS privacy curtain requires AURORA_SUNSHINE_HOST")
    endif()
    find_library(SCREEN_CAPTURE_KIT_LIBRARY ScreenCaptureKit REQUIRED)
    find_library(AURORA_IMAGE_IO_LIBRARY ImageIO REQUIRED)
    set(aurora_privacy_sources
        "${CMAKE_SOURCE_DIR}/src/platform/macos/aurora_privacy.mm"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/aurora_privacy_artwork.mm"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/sc_capture.mm")
    target_sources(${sunshine_engine_target} PRIVATE ${aurora_privacy_sources})
    set_source_files_properties(${aurora_privacy_sources} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(${sunshine_engine_target} PRIVATE AURORA_MACOS_PRIVACY_PREVIEW=1)
    target_link_libraries(${sunshine_engine_target} ${SCREEN_CAPTURE_KIT_LIBRARY} ${AURORA_IMAGE_IO_LIBRARY})
endif()

if (SUNSHINE_BUILD_HOMEBREW)
    target_link_options(sunshine PRIVATE LINKER:-sectcreate,__TEXT,__info_plist,${APPLE_PLIST_FILE})
else()
    # .app build
    set_target_properties(sunshine PROPERTIES
            OUTPUT_NAME "${CMAKE_PROJECT_NAME}"
            MACOSX_BUNDLE_BUNDLE_NAME "${CMAKE_PROJECT_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${PROJECT_FQDN}"
            MACOSX_BUNDLE_INFO_PLIST "${APPLE_PLIST_FILE}"
            MACOSX_BUNDLE_ICON_FILE "sunshine.icns"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

    # Populate bundle resources in the build tree for local runs.
    set(_bundle_resources_dir "$<TARGET_FILE_DIR:sunshine>/../Resources")
    add_custom_command(TARGET sunshine POST_BUILD
            COMMENT "Copying bundle resources to build tree"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle_resources_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_BINARY_DIR}/assets" "${_bundle_resources_dir}/assets"
            VERBATIM)
endif()

# Tell linker to dynamically load these symbols at runtime, in case they're unavailable:
target_link_options(sunshine PRIVATE -Wl,-U,_CGPreflightScreenCaptureAccess -Wl,-U,_CGRequestScreenCaptureAccess)

if(AURORA_SUNSHINE_HOST)
    find_library(AURORA_CORE_SERVICES CoreServices REQUIRED)
    find_library(AURORA_CORE_GRAPHICS CoreGraphics REQUIRED)
    set(aurora_virtual_monitor_source "${CMAKE_SOURCE_DIR}/src/platform/macos/aurora_virtual_monitor.mm")
    target_sources(${sunshine_engine_target} PRIVATE "${aurora_virtual_monitor_source}")
    set_source_files_properties("${aurora_virtual_monitor_source}" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(${sunshine_engine_target} ${AURORA_CORE_SERVICES} ${AURORA_CORE_GRAPHICS})
endif()
