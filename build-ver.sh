#!/bin/sh

echo "#pragma once" > "$1.tmp"
echo "#define VERSION \"$(git describe --tags --abbrev=0 --always | tr - .)\"" >> "$1.tmp"
echo "#define BUILD_DATE \"$(date +'%Y-%m-%d %H:%M')\"" >> "$1.tmp"
echo "#define BUILD_YEAR $(date +%Y)" >> "$1.tmp"
echo "#define BUILD_MONTH $(date +%_m)" >> "$1.tmp"
echo "#define BUILD_DAY $(date +%_d)" >> "$1.tmp"

if ! cmp -s "$1" "$1.tmp" ; then
  mv "$1.tmp" "$1"
fi