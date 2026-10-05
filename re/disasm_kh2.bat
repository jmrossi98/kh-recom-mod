@echo off
rem (KH2 project) usage: disasm_kh2.bat 14016d14c 14016d1c0
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra_kh2" kh2 -process kh2.exe -noanalysis -readOnly -scriptPath "%HERE%ghidra_scripts" -postScript Disasm.java %* > "%HERE%ghidra_kh2\disasm.log" 2>&1
