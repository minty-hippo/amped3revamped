# cmake -DSRC=<file> -DDST=<file> -P copy_if_missing.cmake
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "Created ${DST}")
endif()
