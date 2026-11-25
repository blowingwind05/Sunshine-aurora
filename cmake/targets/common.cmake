# common target definitions
# this file will also load platform specific macros

option(AURORA_SUNSHINE_HOST "Build the host companion managed by Aurora" OFF)
set(sunshine_engine_target sunshine)
set(sunshine_app_sources ${SUNSHINE_TARGET_FILES})
if(AURORA_SUNSHINE_HOST)
    add_library(aurora_sunshine_engine STATIC ${SUNSHINE_TARGET_FILES})
    target_compile_definitions(aurora_sunshine_engine PRIVATE AURORA_SUNSHINE_HOST=1)
    set(sunshine_engine_target aurora_sunshine_engine)
    set(sunshine_app_sources src/AuroraHostMain.cpp src/AuroraHostBootstrap.cpp)
endif()

if(APPLE AND NOT SUNSHINE_BUILD_HOMEBREW)
    add_executable(sunshine MACOSX_BUNDLE ${sunshine_app_sources})
else()
    add_executable(sunshine ${sunshine_app_sources})
endif()
if(AURORA_SUNSHINE_HOST)
    target_link_libraries(sunshine aurora_sunshine_engine)
endif()
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(${sunshine_engine_target} ${dep})
endforeach()

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

target_link_libraries(${sunshine_engine_target} ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
target_compile_definitions(${sunshine_engine_target} PUBLIC ${SUNSHINE_DEFINITIONS})

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SUNSHINE_COMPILE_OPTIONS)
        list(APPEND SUNSHINE_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(${sunshine_engine_target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(sunshine PRIVATE ${SUNSHINE_LINK_OPTIONS})

if(NOT AURORA_SUNSHINE_HOST)
    include("${CMAKE_CURRENT_LIST_DIR}/web-ui.cmake")
endif()

# tests
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# custom compile flags, must be after adding tests

if (NOT BUILD_TESTS)
    set(TEST_DIR "")
else()
    set(TEST_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# third-party/ViGEmClient
set(VIGEM_COMPILE_FLAGS "")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unknown-pragmas ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-misleading-indentation ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-missing-field-initializers ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

# src/nvhttp
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if("${BUILD_TYPE}" STREQUAL "XDEBUG")
    if(WIN32)
        if (NOT BUILD_TESTS)
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}"
                    PROPERTIES COMPILE_FLAGS -O2)
        else()
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests"
                    PROPERTIES COMPILE_FLAGS -O2)
        endif()
    endif()
else()
    add_definitions(-DNDEBUG)
endif()
