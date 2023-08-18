include(ExternalProject)

set(NCURSES_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/ncurses-src)
set(NCURSES_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/ncurses)
set(NCURSES_INCLUDE_DIR ${NCURSES_INSTALL_DIR}/include)
set(NCURSES_CONFIGURE_COMMAND ${NCURSES_SOURCE_DIR}/configure)

ExternalProject_Add(
    ncurses-external
    URL http://ftp.gnu.org/gnu/ncurses/ncurses-6.2.tar.gz
    SOURCE_DIR ${NCURSES_SOURCE_DIR}
    PREFIX ${NCURSES_INSTALL_DIR}
    CONFIGURE_COMMAND ${NCURSES_CONFIGURE_COMMAND}
    --prefix=${NCURSES_INSTALL_DIR}
    BUILD_COMMAND make
    INSTALL_COMMAND make install

    BUILD_ALWAYS 0
    UPDATE_DISCONNECTED 1
)
file(MAKE_DIRECTORY ${NCURSES_INCLUDE_DIR})

add_library(ncurses STATIC IMPORTED)
set_property(TARGET ncurses PROPERTY IMPORTED_LOCATION ${NCURSES_INSTALL_DIR}/lib/libncurses.a)
set_property(TARGET ncurses PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${NCURSES_INCLUDE_DIR})
add_dependencies(ncurses ncurses-external)

set(READLINE_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/readline-src)
set(READLINE_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/readline)
set(READLINE_INCLUDE_DIR ${READLINE_INSTALL_DIR}/include)
set(READLINE_CONFIGURE_COMMAND ${READLINE_SOURCE_DIR}/configure)

ExternalProject_Add(
  readline-external
  SOURCE_DIR ${READLINE_SOURCE_DIR}
  GIT_REPOSITORY https://git.savannah.gnu.org/git/readline.git
  CONFIGURE_COMMAND
  ${READLINE_CONFIGURE_COMMAND}
  --prefix=${READLINE_INSTALL_DIR}
  INSTALL_COMMAND make install-static

  BUILD_ALWAYS 0
  UPDATE_DISCONNECTED 1
)
file(MAKE_DIRECTORY ${READLINE_INCLUDE_DIR})

add_library(readline::readline STATIC IMPORTED)
set_property(TARGET readline::readline PROPERTY IMPORTED_LOCATION ${READLINE_INSTALL_DIR}/lib/libreadline.a)
set_property(TARGET readline::readline PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${READLINE_INCLUDE_DIR})
add_dependencies(readline::readline readline-external)

add_library(readline::history STATIC IMPORTED)
set_property(TARGET readline::history PROPERTY IMPORTED_LOCATION ${READLINE_INSTALL_DIR}/lib/libhistory.a)
set_property(TARGET readline::history PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${READLINE_INCLUDE_DIR})
add_dependencies(readline::history readline-external)

