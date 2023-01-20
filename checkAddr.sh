#!/bin/bash
/opt/tool-chains/sh2eb-elf/bin/sh2eb-elf-addr2line.exe -a $1 -i -p -f -e ./build/cinepak_player.elf
