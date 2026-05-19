@echo off
setlocal

set "SOURCE_DIR=C:\Unluac\Input"
set "TARGET_DIR=C:\Unluac\Output"

:: Create the target directory if it doesn't exist
if not exist "%TARGET_DIR%" mkdir "%TARGET_DIR%"

for %%F in ("%SOURCE_DIR%\*.lua") do (
  echo Decompiling %%F...
  java -jar C:\Unluac\unluac.jar "%%F" > "%TARGET_DIR%\%%~nF.lua"
)

endlocal