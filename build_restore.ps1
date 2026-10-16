. "C:\Espressif\v5.4\esp-idf\export.ps1" *> $null
Set-Location "C:\Projects\esp32-modbus-gateway"
idf.py build 2>&1 | Select-Object -Last 8