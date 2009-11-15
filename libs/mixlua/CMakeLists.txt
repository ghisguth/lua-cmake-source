set(files mixlua.c)

if(WIN32)
  set(files "${files};mixlua.def")
endif(WIN32)

LuaModule(mixlua "${files}")
