# dsync.ps1 - Windows wrapper for dsync.py
# Usage: .\scripts\dsync.ps1 <command> [args] [--force]
# Requires: Python 3.9+ on PATH

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
python "$ScriptDir\dsync.py" @args
