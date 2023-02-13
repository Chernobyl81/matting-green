#!/usr/bin/sh

rm -rf output.yuv
cmake --build build
build/matting-green nv21.yuv output.yuv