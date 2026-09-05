cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBMPC_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_mpc.c
)

SET(LIBMPC_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libmpcdec.a
)

SET(LIBMPC_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libmpc
        "${LIBMPC_SRC}"
        "${LIBMPC_LIBS}"
        []
        ""
        "${LIBMPC_INC}"
        ""
        "1")
