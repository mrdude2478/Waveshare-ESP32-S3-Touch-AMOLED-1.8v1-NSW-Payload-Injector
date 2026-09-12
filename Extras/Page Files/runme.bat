@echo off
rmdir /s /q "done"
if exist "done" (
    echo done folder NOT deleted!
)

set "MyTempFolder=temp"
if not exist "%MyTempFolder%" (
    mkdir "%MyTempFolder%"
)

set "MyDoneFolder=done"
if not exist "%MyDoneFolder%" (
    mkdir "%MyDoneFolder%"
)

binaries\minify -i pages/config.html -o temp/config.html --all
binaries\minify -i pages/editor.html -o temp/editor.html -r "binaries/editor.txt" --all
binaries\minify -i pages/filemanager.html -o temp/filemanager.html -r "binaries/filemanager_replacements.txt" --all
binaries\minify -i pages/fwupdate.html -o temp/fwupdate.html -r "binaries/fwupdate.txt" --all
binaries\minify -i pages/tar.html -o temp/tar.html -r "binaries/tar_replacements.txt" --all
binaries\minify -i pages/index.html -o temp/index.html -r "binaries/index.txt" --minify-css
binaries\minify -i pages/information.html -o temp/information.html -r "binaries/information.txt" --all
binaries\minifycss pages/styles.css temp/styles.css


cd scripts
call 1-Pack-editor.bat
call 2-Pack-fwupdate.bat
call 3-Pack-filemanager.bat
call 4-Pack-config.bat
call 5-Pack-tar.bat
call 6-Pack-index.bat
call 7-Pack-css.bat
call 8-Pack-information.bat
cd ..

rmdir /s /q "temp"
if exist "temp" (
    echo Temp folder NOT deleted!
) else (
    echo Temp folder successfully deleted.
)

echo moving compressed pages header files into include folder.
move /Y done\*.h ..\..\include
rmdir /s /q "done"
pause