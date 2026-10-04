@echo off
rem Imports recom.exe into a Ghidra project and runs auto-analysis.
set "JAVA_HOME=C:\Program Files\Java\jdk-21"
set "HERE=%~dp0"
call "%HERE%..\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%HERE%ghidra" recom -import "%HERE%recom.exe" -overwrite -analysisTimeoutPerFile 3600 > "%HERE%ghidra\analyze.log" 2>&1
