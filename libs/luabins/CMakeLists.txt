set(files "src/load.c;src/luabins.c;src/luainternals.c;src/save.c")

if(WIN32)
  set(files "${files};luabins.def")
endif(WIN32)

if(${WITH_LUAJIT} EQUAL "1")
	add_definitions(-DLUAI_BITSINT=32)
else(${WITH_LUAJIT} EQUAL "1")
	add_definitions(-DLUABINS_LUABUILTASCPP)
endif(${WITH_LUAJIT} EQUAL "1")
LuaModule(luabins "${files}")

