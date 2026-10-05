@echo off
rem usage: findstring_kh2.bat field2d 00battle ...
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra_kh2" kh2 -process kh2.exe -noanalysis -readOnly -scriptPath "%HERE%ghidra_scripts" -postScript FindString.java %* > "%HERE%ghidra_kh2\findstring.log" 2>&1
