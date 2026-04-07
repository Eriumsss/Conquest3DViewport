@echo off
title Wwise FNV Brute Force Attack
cd /d C:\Users\Username\Desktop\Code\ConquestConsole

echo ============================================
echo   Wwise FNV Brute Force Attack Suite
echo ============================================
echo.

echo [1/3] Running Benchmark...
echo.
python wwiseRE/brute_force_advanced.py --benchmark

echo.
echo ============================================
echo [2/3] Running All Safe Attacks (patterns, mitm, suffix)...
echo ============================================
echo.
python wwiseRE/brute_force_advanced.py --all

echo.
echo ============================================
echo [3/3] Running Brute Force (length 1-6)...
echo ============================================
echo.
python wwiseRE/brute_force_advanced.py --brute --min-len 1 --max-len 6

echo.
echo ============================================
echo   Attack Complete!
echo ============================================
pause

