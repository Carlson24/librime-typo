cmake_minimum_required(VERSION 3.10)
project(rime-typo)

aux_source_directory(src typo_src)

add_library(rime-typo-objs OBJECT ${typo_src})

if(BUILD_SHARED_LIBS)
  set_target_properties(rime-typo-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-typo PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-typo-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "typo" PARENT_SCOPE)

include(GNUInstallDirs)

if(NOT DEFINED RIME_DATA_DIR)
    if(UNIX AND NOT APPLE)
        set(RIME_DATA_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/rime-data")
    elseif(APPLE)
        set(RIME_DATA_DIR "/Library/Input Methods/Squirrel.app/Contents/SharedSupport")
    elseif(WIN32)
        set(RIME_DATA_DIR "$ENV{APPDATA}/Rime")
    endif()
    
    message(STATUS "[librime-typo] RIME_DATA_DIR not specified. Auto-routing to: ${RIME_DATA_DIR}")
endif()

install(DIRECTORY data/ DESTINATION "${RIME_DATA_DIR}/typo")