include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

set(files ./src/common.c ./src/pcre/lpcre.c ./src/pcre/lpcre_f.c)

if(WIN32)
  set(files "${files};lrexlib.def")
else(WIN32)
  set(files "${files};./src/posix/lposix.c")
endif(WIN32)

LuaModule(rex "${files}")

if(WIN32)
	target_link_libraries(rex lua ${PCRE_LIBRARIES})
endif(WIN32)
