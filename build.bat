@echo off
set XDK=\\VBOXSVR\XDK
set SRC=\\VBOXSVR\AvatarSrc
set PATH=%XDK%\bin\win32;%PATH%
set INCLUDE=%XDK%\include\xbox;%XDK%\TechPreview\Jul12Compiler\include\xbox;%XDK%\include\xbox\sys
set LIB=%XDK%\lib\xbox;%XDK%\TechPreview\Jul12Compiler\lib\xbox
set LOG=%SRC%\build_out.txt
set WORK=C:\avbuild

if not exist %WORK% mkdir %WORK%
copy /y "%SRC%\main.cpp" "%WORK%\main.cpp" >nul
copy /y "%SRC%\font_seg.h" "%WORK%\font_seg.h" >nul
copy /y "%SRC%\bg_bliss.h" "%WORK%\bg_bliss.h" >nul
cd /d %WORK%
del /q default.xex default.exe main.obj 2>nul

echo === COMPILE (Xenon cl) ===> "%LOG%"
cl.exe /c /nologo /O2 /EHsc /D_XBOX /DNDEBUG /I"%XDK%\include\xbox" /I"%XDK%\TechPreview\Jul12Compiler\include\xbox" /I"%XDK%\include\xbox\sys" main.cpp >> "%LOG%" 2>&1
if not exist main.obj echo COMPILE_FAILED>> "%LOG%"
if not exist main.obj goto done

echo === LINK ===>> "%LOG%"
link.exe /nologo /OUT:default.exe main.obj xavatar2.lib d3d9.lib d3dx9.lib xgraphics.lib xapilib.lib xboxkrnl.lib /LIBPATH:"%XDK%\lib\xbox" >> "%LOG%" 2>&1
if not exist default.exe echo LINK_FAILED>> "%LOG%"
if not exist default.exe goto done

echo === IMAGEXEX (make default.xex) ===>> "%LOG%"
imagexex.exe /nologo /out:default.xex default.exe >> "%LOG%" 2>&1

:done
echo === RESULT ===>> "%LOG%"
dir default.* main.obj >> "%LOG%" 2>&1
if exist default.xex copy /y default.xex "%SRC%\default.xex" >nul
if exist default.xex echo XEX_COPIED_TO_SHARE>> "%LOG%"
