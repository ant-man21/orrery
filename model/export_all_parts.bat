@echo off
REM ============================================================
REM Batch-exports every orrery part as a separate STL, one call
REM to OpenSCAD per part -- no manual uncommenting needed.
REM
REM EDIT THIS: point OPENSCAD at your actual install path if it's
REM not in this default location. Right-click your OpenSCAD
REM shortcut > Properties > Target to find the real path.
REM ============================================================
set OPENSCAD="C:\Program Files\OpenSCAD\openscad.exe"
set FILE=orrery_gear_train.scad
set OUT=stl_output

REM EDIT THIS: set to (your station count - 1). You currently have
REM 4 stations, so indices 0-3 -- change 3 below if you add more.
set MAX_INDEX=3

if not exist %OUT% mkdir %OUT%

echo Exporting base plate...
%OPENSCAD% -o %OUT%\base.stl -D "PART=\"base\"" %FILE%

echo Exporting full-diameter test template...
%OPENSCAD% -o %OUT%\test_template.stl -D "PART=\"template\"" %FILE%

echo Exporting test fitment wedge...
%OPENSCAD% -o %OUT%\test_wedge.stl -D "PART=\"test_wedge\"" %FILE%

echo Exporting sun post...
%OPENSCAD% -o %OUT%\sun.stl -D "PART=\"sun\"" %FILE%

echo Exporting leg...
%OPENSCAD% -o %OUT%\leg.stl -D "PART=\"leg\"" %FILE%

for /L %%i in (0,1,%MAX_INDEX%) do (
    echo Exporting pinion station %%i...
    %OPENSCAD% -o %OUT%\pinion_station%%i.stl -D "PART=\"pinion\"" -D "PART_INDEX=%%i" %FILE%

    echo Exporting idler station %%i...
    %OPENSCAD% -o %OUT%\idler_station%%i.stl -D "PART=\"idler\"" -D "PART_INDEX=%%i" %FILE%

    echo Exporting ring gear station %%i...
    %OPENSCAD% -o %OUT%\ring_station%%i.stl -D "PART=\"ring\"" -D "PART_INDEX=%%i" %FILE%
)

echo.
echo Done. STL files are in the %OUT% folder.
pause
