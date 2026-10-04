@echo off
rem usage: decomp.bat 14015bcb0 14015ba90 ...   -> re\decomp\<addr>.c
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra" recom -process recom.exe -noanalysis -readOnly -scriptPath "%HERE%ghidra_scripts" -postScript Decomp.java "%HERE%decomp" %* > "%HERE%ghidra\decomp.log" 2>&1
