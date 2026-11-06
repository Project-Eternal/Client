# SR-games

The information about the original DOS executables.

To generate an assembler version of the original executable, put the static recompiler (*SR.exe*) and the original executable in the appropriate subdirectory and run *build-x86.sh* (or *build-arm.sh* or *build-llasm.sh* or *build-x64.sh*).

[NASM](https://www.nasm.us/) (Netwide Assembler) is needed to generate (and to compile) the x86 and x64 assembler versions. Versions 2.15.03 -- 2.15.05 contain a bug which prevents them from working with the static recompiler. Use a later (or earlier) version.

The generated assembler version consists of *.asm* (or *.s*) and *.inc* files (or *.llasm* and *.llinc* files). Move these files into the appropriate subdirectory in the *games* subproject.
