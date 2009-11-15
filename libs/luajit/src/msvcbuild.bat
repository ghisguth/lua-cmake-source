@rem Script to build LuaJIT with MSVC.
@rem Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
@rem
@rem Open a "Visual Studio .NET Command Prompt", cd to this directory
@rem and run this script.

@if not defined INCLUDE goto :FAIL

@setlocal
@set LJCOMPILE=cl /nologo /c /MD /O2 /W3 /D_CRT_SECURE_NO_DEPRECATE
@set LJLINK=link /nologo
@set LJMT=mt /nologo
@set DASMDIR=..\dynasm
@set DASM=lua %DASMDIR%\dynasm.lua
@set ALL_LIB=lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c

if not exist buildvm_x86.h^
  %DASM% -LN -o buildvm_x86.h buildvm_x86.dasc

%LJCOMPILE% /I "." /I %DASMDIR% buildvm*.c
%LJLINK% /out:buildvm.exe buildvm*.obj
if exist buildvm.exe.manifest^
  %LJMT% -manifest buildvm.exe.manifest -outputresource:buildvm.exe

buildvm -m peobj -o lj_vm.obj
buildvm -m ffdef -o lj_ffdef.h %ALL_LIB%
buildvm -m libdef -o lj_libdef.h %ALL_LIB%
buildvm -m recdef -o lj_recdef.h %ALL_LIB%
buildvm -m vmdef -o ..\lib\vmdef.lua %ALL_LIB%
buildvm -m folddef -o lj_folddef.h lj_opt_fold.c

@if "%1"=="amalg" goto :AMALGDLL
%LJCOMPILE% /DLUA_BUILD_AS_DLL lj_*.c lib_*.c
%LJLINK% /DLL /out:lua51.dll lj_*.obj lib_*.obj
@goto :MTDLL
:AMALGDLL
%LJCOMPILE% /DLUA_BUILD_AS_DLL ljamalg.c
%LJLINK% /DLL /out:lua51.dll ljamalg.obj lj_vm.obj
:MTDLL
if exist lua51.dll.manifest^
  %LJMT% -manifest lua51.dll.manifest -outputresource:lua51.dll;2

%LJCOMPILE% luajit.c
%LJLINK% /out:luajit.exe luajit.obj lua51.lib
if exist luajit.exe.manifest^
  %LJMT% -manifest luajit.exe.manifest -outputresource:luajit.exe

del *.obj *.manifest buildvm.exe

@goto :END
:FAIL
@echo You must open a "Visual Studio .NET Command Prompt" to run this script
:END
