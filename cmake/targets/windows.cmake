# windows specific target definitions
option(AURORA_SUNSHINE_HOST "Build the Windows host companion managed by Aurora" OFF)
if(AURORA_SUNSHINE_HOST)
    target_compile_definitions(sunshine PRIVATE AURORA_SUNSHINE_HOST=1)
endif()
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib
        version.lib)
