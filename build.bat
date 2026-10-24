@echo off
rem Build QRReader.exe with MinGW-w64 (MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc)
gcc -O2 -c third\quirc\quirc.c third\quirc\decode.c third\quirc\identify.c third\quirc\version_db.c
cd qrreader
echo 1 ICON "app.ico" > app.rc
echo 1 24 "../common/app.manifest" >> app.rc
windres app.rc -O coff -o app.res.o
g++ -O2 -std=c++17 -municode -mwindows -static -s main.cpp app.res.o ..\quirc.o ..\decode.o ..\identify.o ..\version_db.o -ldwmapi -luxtheme -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -o QRReader.exe
cd ..
echo Built qrreader\QRReader.exe
