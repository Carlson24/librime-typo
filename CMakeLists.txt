cmake_minimum_required(VERSION 3.10)
project(rime-typo)

aux_source_directory(src typo_src)

list(REMOVE_ITEM typo_src "src/typo_compiler.cc")

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

if(NOT ANDROID)
  add_executable(typo_compiler src/typo_compiler.cc)
  target_link_libraries(typo_compiler marisa)
endif()

file(GLOB TYPO_TXT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/*.txt")
set(COMPILED_BIN_FILES "")

if(NOT ANDROID)
  foreach(TXT_FILE ${TYPO_TXT_FILES})
      get_filename_component(BASENAME ${TXT_FILE} NAME_WE)
      set(BIN_FILE "${CMAKE_CURRENT_BINARY_DIR}/${BASENAME}.bin")

      add_custom_command(
          OUTPUT ${BIN_FILE}
          COMMAND typo_compiler ${TXT_FILE} ${BIN_FILE}
          DEPENDS ${TXT_FILE} typo_compiler
      )
      list(APPEND COMPILED_BIN_FILES ${BIN_FILE})
  endforeach()

  if(COMPILED_BIN_FILES)
      add_custom_target(build_typo_data ALL DEPENDS ${COMPILED_BIN_FILES})
  endif()

  include(GNUInstallDirs)

  if(NOT DEFINED RIME_DATA_DIR)
      set(RIME_DATA_DIR "rime-data")
  endif()

  if(UNIX AND NOT APPLE)
      set(FINAL_INSTALL_DEST "${CMAKE_INSTALL_DATADIR}/${RIME_DATA_DIR}/build")
  elseif(APPLE)
      set(FINAL_INSTALL_DEST "/Library/Input Methods/Squirrel.app/Contents/SharedSupport/build")
  elseif(WIN32)
      set(FINAL_INSTALL_DEST "$ENV{APPDATA}/Rime/build")
  endif()

  message(STATUS "[librime-typo] Compiled .bin files will be installed to: ${FINAL_INSTALL_DEST}")

  if(COMPILED_BIN_FILES)
      install(FILES ${COMPILED_BIN_FILES} DESTINATION "${FINAL_INSTALL_DEST}")
  endif()
endif()
