@echo off
chcp 65001 >nul
echo ========================================
echo   以太网代码恢复脚本
echo   将 Ethernet_Backup 覆盖回 APP_LED
echo ========================================
echo.

set "SRC=Ethernet_Backup"
set "DST=BootLoader+APP\APP_LED"

echo [1/4] 恢复 Core 文件...
xcopy "%SRC%\Core\*" "%DST%\Core\" /E /Y /Q
if %errorlevel% neq 0 (echo 错误: Core 恢复失败! & pause & exit /b 1)

echo [2/4] 恢复 LWIP 文件...
xcopy "%SRC%\LWIP\*" "%DST%\LWIP\" /E /Y /Q
if %errorlevel% neq 0 (echo 错误: LWIP 恢复失败! & pause & exit /b 1)

echo [3/4] 恢复 DOIP 文件...
xcopy "%SRC%\DOIP\*" "%DST%\DOIP\" /E /Y /Q
if %errorlevel% neq 0 (echo 错误: DOIP 恢复失败! & pause & exit /b 1)

echo [4/4] 恢复 PHY 驱动...
xcopy "%SRC%\Drivers\*" "%DST%\Drivers\BSP\" /E /Y /Q
if %errorlevel% neq 0 (echo 错误: PHY 驱动恢复失败! & pause & exit /b 1)

echo.
echo ========================================
echo   全部恢复完成!
echo ========================================
pause
