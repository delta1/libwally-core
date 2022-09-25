#!/bin/bash

include_dir=${1:?"Usage: $0 <path-to-libwally-include-dir>"}

echo '// AUTOGENERATED by `npm run update-consts`'

grep -r '#define WALLY_.* ' $include_dir \
    | sed -r 's~.*#define (WALLY_[^ ]*) *~export const \1 = ~; s~( /\*)| *$~;\1~'
