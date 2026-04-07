#!/bin/bash

cargo build --release
cp target/release/lotrc_rs ./lotrc_rs.bin
cp target/release/liblotrc.so ./lotrc.so
rm lotrc_blender_linux.zip
pushd lotrc_blender
zip -r ../lotrc_blender_linux.zip * -i*.*
popd
zip lotrc_blender_linux.zip lotrc.so
