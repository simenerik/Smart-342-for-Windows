#!/bin/sh
# Cross-compile the Windows x64 programs (mingw-w64).
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
CF="-O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function"
CORE="fsu.c ppc.c pef.c emu.c text.c world.c hle_core.c hle_ae.c hle_files.c hle_scsi.c sim.c guard.c fwmem.c replay.c fghooks.c popups.c rawscan.c cassette.c ui_stub.c hle_ui.c coro.c msys.c"
APPX="minerbar.c rawstack.c rawhdr.c rawtile.c rawcmp.c rawsr.c rawsharp.c rawreg.c prefscopy.c rawchart.c cleangain.c streakcal.c negconv.c"
mkdir -p ../build-win
$CC $CF -o ../build-win/run_sys.exe $CORE spti.c run_sys.c -lm || exit 1
echo built ../build-win/run_sys.exe
x86_64-w64-mingw32-windres app.rc -O coff -o ../build-win/app.res || exit 1
$CC $CF -municode -mwindows -o ../build-win/smart342.exe $CORE $APPX spti.c app.c ../build-win/app.res -lm -lcomdlg32 -lshell32 -lcomctl32 || exit 1
echo built ../build-win/smart342.exe
