set(files lpeg.c)

if(WIN32)
  set(files "${files};lpeg.def")
endif(WIN32)

LuaModule(lpeg "${files}")
