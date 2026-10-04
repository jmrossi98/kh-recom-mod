@echo off
rem usage: xrefs.bat 14087b390 14087a650 ...
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra" recom -process recom.exe -noanalysis -readOnly -scriptPath "%HERE%ghidra_scripts" -postScript Xrefs.java %* > "%HERE%ghidra\xrefs.log" 2>&1
