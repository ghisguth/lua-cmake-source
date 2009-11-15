macro(dobuildvm action output params)
	add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${output}
		COMMAND ${EXECUTABLE_OUTPUT_PATH}/buildvm
		ARGS -m ${action} -o ${CMAKE_CURRENT_BINARY_DIR}/${output} ${params}
		DEPENDS buildvm ${params}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		COMMENT "buildvm ${action} ${output} ${params}")
	add_custom_target(buildvm_${action} ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${output})
endmacro(dobuildvm action output params)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

file(GLOB buildvm_files buildvm*.c)
add_executable(buildvm ${buildvm_files})

set(all_lib lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c)
dobuildvm(asm lj_vm.s "")
dobuildvm(ffdef lj_ffdef.h "${all_lib}")
dobuildvm(libdef lj_libdef.h "${all_lib}")
dobuildvm(recdef lj_recdef.h "${all_lib}")
#dobuildvm(vmdef ../lib/vmdef.lua "${all_lib}")
dobuildvm(folddef lj_folddef.h lj_opt_fold.c)

add_definitions(-U_FORTIFY_SOURCE -fno-stack-protector -D_FILE_OFFSET_BITS=64)

file(GLOB lua_files lj_*.c lib_*.c)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lj_vm.o
	COMMAND gcc
	ARGS -c -o ${CMAKE_CURRENT_BINARY_DIR}/lj_vm.o ${CMAKE_CURRENT_BINARY_DIR}/lj_vm.s
	DEPENDS buildvm ${CMAKE_CURRENT_BINARY_DIR}/lj_vm.s
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMENT "compiling lj_vm.s")

add_library(luajit SHARED ${lua_files} ${CMAKE_CURRENT_BINARY_DIR}/lj_vm.o)
set_target_properties(luajit PROPERTIES LINKER_LANGUAGE C)

add_dependencies(luajit buildvm_asm buildvm_ffdef buildvm_libdef buildvm_recdef buildvm_vmdef buildvm_folddef)

add_executable(luaexe luajit.c)
#target_link_libraries(luaexe m dl luajit)
set_target_properties(luaexe PROPERTIES LINKER_LANGUAGE "C" OUTPUT_NAME "lua")

set(compile_flags "")
set(link_flags "")
set(definitions "")

if(UNIX)
	#set(compile_flags ${compile_flags} "-x c++")
	if(APPLE)
		set(LIBS ${LIBS} readline)
		set(definitions "${definitions} -DLUA_USE_MACOSX -DLUA_USE_DLOPEN -DLUA_USE_READLINE")
		set(link_flags ${link_flags} "-dynamiclib -undefined dynamic_lookup")
	else(APPLE)
		set(definitions ${definitions} -DLUA_USE_LINUX)
		set(LIBS ${LIBS} luajit m dl)
		set(link_flags ${link_flags} "-Wl,-E")
	endif(APPLE)
else(UNIX)
	if(WIN32)
		set(definitions "${definitions} -DLUA_BUILD_AS_DLL")
		set(definitions "${definitions} -D_CRT_SECURE_NO_DEPRECATE -D_CRT_NONSTDC_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS")
		set(compile_flags ${compile_flags} "/TP")
	endif(WIN32)
endif(UNIX)

if(NOT ${definitions} EQUAL "")
	add_definitions(${definitions})
endif(NOT ${definitions} EQUAL "")

if(NOT ${compile_flags} EQUAL "")
	set_target_properties(luaexe luajit PROPERTIES COMPILE_FLAGS ${compile_flags})
endif(NOT ${compile_flags} EQUAL "")
if(NOT ${link_flags} EQUAL "")
	set_target_properties(luaexe luajit PROPERTIES LINK_FLAGS ${link_flags})
endif(NOT ${link_flags} EQUAL "")

target_link_libraries(luaexe ${LIBS})
