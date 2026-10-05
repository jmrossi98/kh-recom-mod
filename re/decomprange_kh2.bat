@echo off
rem (KH2 project) usage: decomprange_kh2.bat 140150000 140170000   -> re\decomp\<addr>.c
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra_kh2" kh2 -process kh2.exe -noanalysis -readOnly -scriptPath "%HERE%ghidra_scripts" -postScript DecompRange.java "%HERE%decomp_kh2" %* > "%HERE%ghidra_kh2\decomp.log" 2>&1
