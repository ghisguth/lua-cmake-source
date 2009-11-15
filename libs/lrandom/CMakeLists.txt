
set(files "lrandom.c;random.c")
if(WIN32)
  set(files "${files};lrandom.def")
endif(WIN32)

LuaModule(lrandom "${files}")

set_target_properties(lrandom PROPERTIES LINKER_LANGUAGE "CXX" OUTPUT_NAME "random")
