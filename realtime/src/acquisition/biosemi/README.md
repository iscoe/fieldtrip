Biosemi acqusition tool
==========================

Building on Windows
---------------------
1. Install the USB driver from http://www.biosemi.com/download.htm
2. Install MinGW 32 bit from http://www.mingw.org/
3. Add the MinGW bin directory to your system path
4. Open a command prompt and navigate to realtime\src\buffer\src
5. Run: mingw32-make.exe
6. Navigate to realtime\buffer\cpp and run mingw32-make.exe
7. Navigate to realtime\acquisition\biosemi and run mingw32-make.exe

This should build a new version of biosemi2ft.exe in realtime\bin\win32. Check 
the timestamp on that file to make sure that it was successfully built.

The build process produces many warnings and a few non-fatal failures. You can
ignore the warnings and messages like "process_begin: CreateProcess(NULL, uname, ...) failed."
A true fatal error will stop the build process with an error message like
"mingw32-make: *** [rdaserver.o] Error 1"

Running
-------------------
