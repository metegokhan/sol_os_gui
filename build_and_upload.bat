@echo off
echo Derleme baslatiliyor...
python -m platformio run -e waveshare_esp32_s3_rlcd_4_2
if %errorlevel% neq 0 (
    echo.
    echo [HATA] Derleme basarisiz oldu! Yukleme iptal edildi.
    pause
    exit /b %errorlevel%
)

echo.
echo Yukleme baslatiliyor (Flaslama)...
python -m esptool --chip esp32s3 --port COM7 --baud 460800 write-flash 0x20000 .pio\build\waveshare_esp32_s3_rlcd_4_2\firmware.bin
if %errorlevel% neq 0 (
    echo.
    echo [HATA] Yukleme basarisiz oldu! COM7 portunun baska bir program (serial monitor vb.) tarafindan kullanilmadigindan emin olun.
    pause
    exit /b %errorlevel%
)

echo.
echo [BASARILI] Derleme ve Yukleme tamamlandi!
pause
