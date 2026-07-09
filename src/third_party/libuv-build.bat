REM builds libuv as a static library (Release x64) and places libuv.lib where the
REM EventRegistry projects expect it: libuv\x64\Release\libuv.lib
REM requires cmake and Visual Studio; run from src\third_party\
cmake -S libuv -B libuv\build -A x64 -DLIBUV_BUILD_TESTS=OFF -DLIBUV_BUILD_SHARED=OFF
cmake --build libuv\build --config Release
if not exist libuv\x64\Release mkdir libuv\x64\Release
copy /Y libuv\build\Release\libuv.lib libuv\x64\Release\libuv.lib
