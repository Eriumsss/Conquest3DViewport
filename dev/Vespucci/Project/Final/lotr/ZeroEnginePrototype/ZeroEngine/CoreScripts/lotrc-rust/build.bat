cargo build --release
cargo +nightly build -Z build-std --target x86_64-win7-windows-msvc --release

echo F | xcopy /y "target\release\lotrc_rs.exe" "lotrc_rs.exe"
echo F | xcopy /y "target\x86_64-win7-windows-msvc\release\lotrc_rs.exe" "lotrc_rs_win7.exe"
echo F | xcopy /y "target\release\lotrc.dll" "lotrc.pyd"
echo F | xcopy /y "target\x86_64-win7-windows-msvc\release\lotrc.dll" "lotrc_win7.pyd"
