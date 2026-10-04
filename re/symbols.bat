@echo off
rem usage: symbols.bat CardBattle vftable
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra" recom -process recom.exe -noanalysis -readOnly -scriptPath "%HERE%ghidra_scripts" -postScript Symbols.java %* > "%HERE%ghidra\symbols.log" 2>&1
