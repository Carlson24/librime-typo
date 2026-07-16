project(rime-typo)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src typo_src)

add_library(rime-typo-objs OBJECT ${typo_src})

target_include_directories(rime-typo-objs PRIVATE ${CMAKE_SOURCE_DIR}/librime/deps/marisa-trie/include)

if(BUILD_SHARED_LIBS)
  set_target_properties(rime-typo-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-typo PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-typo-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} marisa PARENT_SCOPE)
set(plugin_modules "typo" PARENT_SCOPE)
