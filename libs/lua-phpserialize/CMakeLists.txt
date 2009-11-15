set(files "src/util.c;src/lua-phpserialize.c")

if(WIN32)
  set(files "${files};src/lua-phpserialize.def")
endif(WIN32)

LuaModule(lua-phpserialize "${files}")
