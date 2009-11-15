set(files "util.c;lphpserialize.c")

if(WIN32)
  set(files "${files};lphpserialize.def")
endif(WIN32)

LuaModule(lphpserialize "${files}")
