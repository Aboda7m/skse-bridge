@echo off
rem Builds tools\dist\SKSE-Bridge-Viz.exe (one file). Needs Python 3.9+ on PATH. Run from the tools folder.
rem Usage of the exe: drag a SKSE folder / SKSE-Bridge.log / trace .jsonl / .zip onto it, or run it with no arguments
rem to process Documents\My Games\Skyrim Special Edition*\SKSE (current log + SKSE-Bridge-history).
rem The video step uses the Edge or Chrome that is already installed (no download); without one you still get replay.html.
py -m pip install --upgrade pyinstaller playwright || goto :err
py -m PyInstaller --onefile --name SKSE-Bridge-Viz --collect-all playwright --add-data "trace_replay.html;." --add-data "log_to_trace.py;." bridge_viz.py || goto :err
echo Built: dist\SKSE-Bridge-Viz.exe
goto :eof
:err
echo Build failed.
exit /b 1
