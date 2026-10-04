@echo off
rem Double-click to set everything up: see tools\setup.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
