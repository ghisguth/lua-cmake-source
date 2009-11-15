set(files bit.c)

if(WIN32)
  set(files "${files};bit.def")
endif(WIN32)

LuaModule(bit "${files}")
