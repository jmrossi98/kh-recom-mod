@echo off
rem Imports kh2.exe (KH2 Final Mix from the same collection) into its own Ghidra project.
rem KH2 is the reference for everything this mod imitates.
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra_kh2" kh2 -import "%HERE%kh2.exe" -overwrite -analysisTimeoutPerFile 3600 > "%HERE%ghidra_kh2\analyze.log" 2>&1
