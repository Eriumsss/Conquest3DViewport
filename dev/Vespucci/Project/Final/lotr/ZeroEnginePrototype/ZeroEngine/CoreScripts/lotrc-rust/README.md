# Lord of the Rings Conquest Level Parsing

Consists of a rust library for interacting with LOTRC files and a compiled tool for basic usage.

The library is also compiled for as a python module which should work in python 3.9 or greater. This is bundled with the experimental blender add-on.

For the deprecated python library see https://github.com/haighcam/lotrc/tree/main

Command line utility for converting some Lord of the Rings Conquest files to and from a more editable format.
Currently supports:
- Level Data (.PAK/.BIN)
- level\_info.dat
- WWiseIDTable.bin

Has somewhat sane default behavior for dragging and dropping files/folders onto the executable if command line parameters are not required.
> An args.txt file can be placed in the same folder as the executable to provide additional arguments when the command line is not used.
> When using the tool by dragging and dropping files, any error messages will generally not persist long enough for them to be read. To get around this a script can be used to wait after running the tool, eg: [lotrc.bat](examples/lotrc.bat).

Uses [_lua-bytecode_](https://github.com/lua-bytecode/lua-bytecode.github.io) (included as a submodule), for converting lua files. This is only relevant for converting xbox level files.

Can use [_unluac.jar_](https://sourceforge.net/projects/unluac/) for decompiling lua files.

level\_info, WWiseIDTable and the sub\_blocks inside of levels should be fully editable. Other aspects can be modified but may break the resulting level if modified incorrectly. Python scripts to help with swapping some objects between levels are provided in the examples folder. The provided scripts should work with levels dumped as zip files or as folders.

# Command line usage
```
Usage: lotrc_rs.exe [OPTIONS] [INPUT]...

Arguments:
  [INPUT]...  Input files or folders

Options:
  -o, --output <OUTPUT>            Output folder
  -c, --compile                    Compile the inputs to new levels / level_infos
  -d, --dump                       Dump the inputs to an editable form
  -k, --hash                       Convert input strings into CRCs
      --lua-decomp                 Decompile lua files when loading a level
      --lua-recomp                 Compile lua files when loading a level, also converts endianess for xbox lua files
      --compression <COMPRESSION>  Zlib compression level to use when compiling levels, lower numbers are faster
      --unluac <UNLUAC>            Path to unluac.jar if decompiling lua files
  -a, --no-anim-table              Don't dump animation tables
  -z, --zip                        Dump to zip files instead of folders
      --gltf                       Dump models as gltfs
      --alt-objs                   Dump / Load GameObjs blocks in alternate format
  -h, --help                       Print help
  -V, --version                    Print version
```
simple examples:
 - dump Minas Tirith Top:    
 ```bash lotrc_rs.exe -d -o DumpedLevels 'The Lord of the Rings Conquest 2\Levels\MinasTirith_Top'```
 - compile Minas Tirith Top:  
 ```bash lotrc_rs.exe -c -o 'The Lord of the Rings Conquest 2\Levels' DumpedLevels\MinasTirith_Top```
 - dump all levels:  
 ```bash lotrc_rs.exe -d -o DumpedLevels 'The Lord of the Rings Conquest 2\Levels\*'```
 - compile all levels:  
 ```bash lotrc_rs.exe -c -o 'The Lord of the Rings Conquest 2\Levels' DumpedLevels\*```


# Buffer size warnings
If the tool gives a warning about buffer sizes being too small then the level will crash with an unmodified conquest executable.

For testing if you are using a debugger you can modify the values at 00a3e200 and 00a3e204 to change the buffer sizes for texture and vertex data respectively (after the initialization has set their updated values and before they are read for constructing the buffers). A more permanent solution would be to modify c70500e2a3000000a00a in the hex of the executable to c70500e2a300 + desired size for texture data and c70504e2a30000006006 to c70504e2a300 + desired size for vertex data. (For example c70500e2a3000000a00a -> c70500e2a30000000010 would give 268435456 bytes for texture data).

# Compiling from source
Requires rust and cargo. Some of the used crates require cmake and possibly other dependencies.
```bash
git clone -b rust --single-branch --recurse-submodules https://github.com/haighcam/lotrc.git lotrc_rs
cd lotrc_rs
cargo build --release 
```
