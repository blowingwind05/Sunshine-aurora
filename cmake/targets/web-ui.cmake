# Standalone Sunshine browser interface.

# Homebrew build fails the vite build if we set these environment variables
if(${SUNSHINE_BUILD_HOMEBREW})
    set(NPM_SOURCE_ASSETS_DIR "")
    set(NPM_ASSETS_DIR "")
    set(NPM_BUILD_HOMEBREW "true")
else()
    set(NPM_SOURCE_ASSETS_DIR ${SUNSHINE_SOURCE_ASSETS_DIR})
    set(NPM_ASSETS_DIR ${CMAKE_BINARY_DIR})
    set(NPM_BUILD_HOMEBREW "")
endif()

#WebUI build
find_program(NPM npm REQUIRED)

if(WIN32)
    get_filename_component(NPM_DIRECTORY "${NPM}" DIRECTORY)
    find_program(NPM_NODE_EXECUTABLE NAMES node node.exe HINTS "${NPM_DIRECTORY}" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    string(CONCAT NPM_NODE_GNU_BINDING_CHECK
            "process.arch === 'x64' && "
            "(process.config.variables.shlib_suffix === 'dll.a' || "
            "process.config.variables.node_target_type === 'shared_library')")
    execute_process(
            COMMAND "${NPM_NODE_EXECUTABLE}" -p "${NPM_NODE_GNU_BINDING_CHECK}"
            OUTPUT_VARIABLE NPM_NODE_USES_GNU_BINDING
            OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NPM_NODE_USES_GNU_BINDING STREQUAL "true")
        find_program(NATIVE_NPM NAMES npm.cmd npm HINTS "$ENV{ProgramFiles}/nodejs" NO_DEFAULT_PATH NO_CACHE)
        if(NOT NATIVE_NPM)
            message(FATAL_ERROR
                    "The MSYS2 Node.js package is incompatible with Rolldown. "
                    "Install native Windows Node.js or set NPM to its npm.cmd path.")
        endif()

        set(NPM "${NATIVE_NPM}" CACHE FILEPATH "Path to the npm executable" FORCE)
        get_filename_component(NPM_DIRECTORY "${NPM}" DIRECTORY)
        message(STATUS "MSYS2 Node.js is incompatible with Rolldown; using native npm: ${NPM}")
    endif()

    set(NPM_COMMAND cmd /C)
    set(NPM_PATH "PATH=${NPM_DIRECTORY};$ENV{PATH}")
else()
    set(NPM_COMMAND)
    set(NPM_PATH "PATH=$ENV{PATH}")
endif()

set(NPM_INSTALL_FLAGS "--ignore-scripts")
if (NPM_OFFLINE)
    set(NPM_INSTALL_FLAGS "${NPM_INSTALL_FLAGS} --offline")
endif()

add_custom_target(web-ui ALL
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Installing NPM Dependencies and Building the Web UI"
        COMMAND "${CMAKE_COMMAND}" -E env "${NPM_PATH}" ${NPM_COMMAND} "${NPM}" ci ${NPM_INSTALL_FLAGS}
        COMMAND "${CMAKE_COMMAND}" -E env "${NPM_PATH}" "SUNSHINE_BUILD_HOMEBREW=${NPM_BUILD_HOMEBREW}" "SUNSHINE_SOURCE_ASSETS_DIR=${NPM_SOURCE_ASSETS_DIR}" "SUNSHINE_ASSETS_DIR=${NPM_ASSETS_DIR}" ${NPM_COMMAND} "${NPM}" run build  # cmake-lint: disable=C0301
        VERBATIM)
