@echo off
cd /d "%~dp0"
"%USERPROFILE%\.local\bin\uv.exe" run midi_ble_bridge.py
