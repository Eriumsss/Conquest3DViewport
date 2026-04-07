#!/bin/bash

cargo xwin build --target x86_64-pc-windows-msvc --release
cargo +nightly xwin build -Z build-std --target x86_64-win7-windows-msvc --release

cp target/x86_64-pc-windows-msvc/release/lotrc_rs.exe ./lotrc_rs.exe
cp target/x86_64-win7-windows-msvc/release/lotrc_rs.exe ./lotrc_rs_win7.exe
cp target/release/liblotrc.so ./lotrc.so
rm lotrc_blender.zip
rm lotrc_blender_win7.zip
pushd lotrc_blender
zip -r ../lotrc_blender.zip * -i*.*
zip -r ../lotrc_blender_win7.zip * -i*.*
popd
cp target/x86_64-pc-windows-msvc/release/lotrc.dll ./lotrc.pyd
zip lotrc_blender.zip lotrc.pyd
cp target/x86_64-win7-windows-msvc/release/lotrc.dll ./lotrc.pyd
zip lotrc_blender_win7.zip lotrc.pyd
