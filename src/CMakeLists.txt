include_directories(${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
   add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN -DWINVER=0x0400 -D_WIN32_WINNT=0x0400)
endif(WIN32)

if(${WITH_LUAJIT} EQUAL "1")
	add_definitions(-DWITHLUAJIT)
endif(${WITH_LUAJIT} EQUAL "1")

subdirs(test)
