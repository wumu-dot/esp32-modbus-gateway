. "C:\Espressif\v5.4\esp-idf\export.ps1" *> $null
Set-Location "C:\Projects\esp32-modbus-gateway"
idf.py -p COM9 monitor 2>&1 | Tee-Object "C:\Projects\esp32-modbus-gateway\th_monitor.log"