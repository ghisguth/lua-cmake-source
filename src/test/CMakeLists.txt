include_directories(${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB test_files *.cpp *.hpp)
add_executable(test  ${test_files})

set(ADD_LIBS "")

if(${WITH_LUAJIT} EQUAL "1")
	set(ADD_LIBS ${ADD_LIBS} luajit)
else(${WITH_LUAJIT} EQUAL "1")
	set(ADD_LIBS ${ADD_LIBS} lua)
endif(${WITH_LUAJIT} EQUAL "1")

if(NOT WIN32)
	set(ADD_LIBS ${ADD_LIBS} m dl)
endif(NOT WIN32)
target_link_libraries(test ${ADD_LIBS})

if(WIN32)
  set_target_properties(test PROPERTIES PREFIX "../")
else(WIN32)
  set_target_properties(test PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${PROJECT_BINARY_DIR}" BUILD_WITH_INSTALL_RPATH "1")
endif(WIN32)
