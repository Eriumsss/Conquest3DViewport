use std::{
    collections::{HashSet, VecDeque}, fs, path::{Path, PathBuf}, io::Read,
};
use log::error;
use clap::{Parser, Args};
use anyhow::Result;
use indicatif::MultiProgress;

use lotrc::{
    audio::AudioTable, 
    level::Level,
    level_alt::Level as LevelAlt,
    level_info::LevelInfo,
    read_write::{Reader, Writer, PathStuff},
    shader::Shaders,
    types::{PC, Crc, DECOMP_LUA, RECOMP_LUA, ZIP, ANIM_TABLES, GLTF, COMPRESSION, UNLUAC, ALT_OBJS},
};

fn v3_styling() -> clap::builder::styling::Styles {
    use clap::builder::styling::*;
    Styles::styled()
        .header(clap::builder::styling::AnsiColor::Yellow.on_default())
        .usage(AnsiColor::Green.on_default())
        .literal(AnsiColor::Green.on_default())
        .placeholder(AnsiColor::Green.on_default())
}

#[derive(Parser, Debug, Default)]
#[command(version, about, long_about = None, styles=v3_styling())]
struct CliArgs {
    /// Input files or folders
    #[arg(num_args = 1..)]
    input: Vec<String>,

    /// Output folder
    #[arg(short, long)]
    output: Option<String>,

    #[command(flatten)]
    command: Commands,

    /// Decompile lua files when loading a level
    #[arg(long)]
    lua_decomp: bool,

    /// Compile lua files when loading a level, also converts endianess for xbox lua files
    #[arg(long)]
    lua_recomp: bool,

    /// Zlib compression level to use when compiling levels, lower numbers are faster
    #[arg(long, value_parser = clap::value_parser!(u32).range(0..10))]
    compression: Option<u32>,

    /// Path to unluac.jar if decompiling lua files
    #[arg(long)]
    unluac: Option<String>,

    /// Don't dump animation tables
    #[arg(short='a', long)]
    no_anim_table: bool,

    /// Dump to zip files instead of folders 
    #[arg(short='z', long)]
    zip: bool,

    /// Dump models as gltfs
    #[arg(long)]
    gltf: bool,

    /// Dump / Load GameObjs blocks in alternate format
    #[arg(long)]
    alt_objs: bool,
}

impl CliArgs {
    fn combine(self, other: Self) -> Self {
        Self {
            input: self.input.into_iter().chain(other.input).collect(),
            output: self.output.or(other.output),
            command: self.command.combine(other.command),
            lua_decomp: self.lua_decomp || other.lua_decomp,
            lua_recomp: self.lua_recomp || other.lua_recomp,
            compression: self.compression.or(other.compression),
            unluac: self.unluac.or(other.unluac),
            no_anim_table: self.no_anim_table || other.no_anim_table,
            zip: self.zip || other.zip,
            gltf: self.gltf || other.gltf,
            alt_objs: self.alt_objs || other.alt_objs
        }
    }
}

#[derive(Args, Debug, Default)]
#[group(required = false, multiple = false)]
struct Commands {
    /// Compile the inputs to new levels / level_infos
    #[arg(short, long)]
    compile: bool,

    /// Dump the inputs to an editable form
    #[arg(short, long)]
    dump: bool,

    /// Convert input strings into CRCs
    #[arg(short='k', long)]
    hash: bool,

    #[arg(long, hide=true)]
    alt_comp: bool,
}

impl Commands {
    fn combine(self, other: Self) -> Self {
        if self.compile || self.dump || self.hash || self.alt_comp {
            self
        } else {
            other
        }
    }
}

fn parse<A: AsRef<Path>, B: AsRef<Path>>(src: A, dest: B, args: &Commands, parsed: &mut HashSet<PathBuf>, mp: Option<&MultiProgress>) -> Result<()> {
    let mut q = VecDeque::from(vec![(PathBuf::new(), src.as_ref().to_path_buf())]);
    let dest = dest.as_ref();
    while let Some((name, src)) = q.pop_front() {
        let mut raw_name = src.file_name().unwrap().to_str().unwrap().split('.');
        let name = name.join(raw_name.next().unwrap());
        let ext = raw_name.collect::<Vec<_>>().join(".");
        if src.with_extension("PAK").is_file() && !parsed.contains(&src.with_extension("PAK")) {
            parsed.insert(src.with_extension("PAK"));
            match args {
                Commands { compile: true, .. } => Level::parse(src)?.dump::<PC, _>(dest.join(name))?,
                Commands { alt_comp: true, .. } => LevelAlt::parse(src, mp)?.dump::<PC, _>(dest.join(name), mp)?,
                _ => LevelAlt::parse(src, mp)?.to_file(Writer::new(dest.join(name), *ZIP.lock().unwrap())?, mp.cloned())?,
            }
        } else if src.file_name().unwrap() == "level_info.dat" {
            parsed.insert(src.clone());
            let level_info = LevelInfo::parse(src)?;
            match args {
                Commands { compile: true, .. } => level_info.dump::<PC, _>(dest.join(name))?,
                _ => level_info.to_file(Writer::new(dest.join(name), *ZIP.lock().unwrap())?)?,
            }
        } else if !src.with_extension("PAK").is_file() && src.with_extension("bin").is_file() && ext == "bin" {
            parsed.insert(src.with_extension("bin"));
            let mut vals = vec![0;4];
            {
                let mut f = fs::File::open(&src)?;
                f.read_exact(&mut vals)?;
            }
            if vals[0] == 1 || vals[3] == 1 {
                let shaders = Shaders::parse(src)?;
                match args {
                    Commands { compile: true, .. } => shaders.dump::<PC, _>(dest.join(name))?,
                    _ => shaders.to_file(Writer::new(dest.join(name), *ZIP.lock().unwrap())?)?,
                }
            } else if vals[0] == 2 || vals[3] == 2 {
                let table = AudioTable::parse(src)?;
                match args {
                    Commands { compile: true, .. } => table.dump::<PC, _>(dest.join(name)),
                    _ => table.to_file(dest.join(name)),
                }
            } else {
                error!("Unhandled .bin filetype {}", src.display());
            }
        } else if ext == "audio.json" {
            parsed.insert(src.clone());
            let table = AudioTable::from_file(src)?;
            match args {
                Commands { dump: true, .. } => table.to_file(dest.join(name)),
                _ => table.dump::<PC, _>(dest.join(name)),
            }
        } else if {
            if let Some(reader) = (ext == "zip").then(|| Reader::new(&src, true))
                .or(src.is_dir().then(|| Reader::new(&src, false))) {
                let reader = reader?;
                let name = name.clone();
                if reader.join("index.json").is_file() {
                    let level_info = LevelInfo::from_file(reader)?;
                    match args {
                        Commands { dump: true, .. } => level_info.to_file(Writer::new(dest.join(name), *ZIP.lock().unwrap())?)?,
                        _ => level_info.dump::<PC, _>(dest.join(name))?,
                    }
                    true
                } else if reader.join("pak_header.json").is_file() {
                    let level = LevelAlt::from_file(reader, mp)?;
                    match args {
                        Commands { dump: true, .. } => level.to_file(Writer::new(dest.join(name), *ZIP.lock().unwrap())?, mp.cloned())?,
                        _ => level.dump::<PC, _>(dest.join(name), mp)?
                    }
                    true
                } else if reader.join("vertex_headers.json").is_file() {
                    let shaders = Shaders::from_file(reader)?;
                    match args {
                        Commands { dump: true, .. } => shaders.to_file(Writer::new(dest.join(name), *ZIP.lock().unwrap())?)?,
                        _ => shaders.dump::<PC, _>(dest.join(name))?,
                    }
                    true
                } else {
                    false
                }
            } else {
                false
            }
        } {
            parsed.insert(src.clone());
        } else if src.is_dir() {
            parsed.insert(src.clone());
            for path in fs::read_dir(&src).unwrap().map(|x| x.unwrap().path()) {
                q.push_back((name.clone(), path));
            }
        } else if ext != "bnk" && ext != "arg" && !parsed.contains(&src.with_extension("PAK")) {
            error!("Could not parse input {:?}", src);
        }
    }
    Ok(())
}

fn main() -> Result<()> {
    let logger = pretty_env_logger::formatted_builder()
        .filter_level(log::LevelFilter::Info)
        .format(|buf, record| {
            use std::io::Write;
            use pretty_env_logger::env_logger::fmt::Color;
        
            let mut style = buf.style();
            let level = match record.level() {
                log::Level::Trace => style.set_color(Color::Magenta).value("TRACE"),
                log::Level::Debug => style.set_color(Color::Blue).value("DEBUG"),
                log::Level::Info => style.set_color(Color::Green).value("INFO "),
                log::Level::Warn => style.set_color(Color::Yellow).value("WARN "),
                log::Level::Error => style.set_color(Color::Red).value("ERROR"),
            };
            writeln!(buf, " {} > {}", level, record.args())
        })
        .build();

    let level = logger.filter();
    let multi = indicatif::MultiProgress::new();
    indicatif_log_bridge::LogWrapper::new(multi.clone(), logger).try_init().unwrap();
    log::set_max_level(level);

    let exe_dir = std::env::current_exe().unwrap().parent().unwrap().to_owned();
    let mut args = CliArgs::parse_from(wild::args_os());
    for arg_file in args.input.clone().iter()
        .filter(|x| x.ends_with(".arg"))
        .map(|x| PathBuf::from(x))
        .chain(std::iter::once(exe_dir.join("args.txt")))
        .filter(|x| x.is_file()) {

        let data = fs::read_to_string(arg_file)?;
        args = args.combine(CliArgs::parse_from(std::iter::once("").chain(data.as_str().split_whitespace())));
    }
    let unluac = exe_dir.join("unluac.jar");
    if unluac.is_file() {
        args.unluac = args.unluac.or(Some(unluac.to_string_lossy().to_string()))
    }

    *DECOMP_LUA.lock().unwrap() = args.lua_decomp;
    *RECOMP_LUA.lock().unwrap() = args.lua_recomp;
    *ANIM_TABLES.lock().unwrap() = !args.no_anim_table;
    *ZIP.lock().unwrap() = args.zip;
    *GLTF.lock().unwrap() = args.gltf;
    *ALT_OBJS.lock().unwrap() = args.alt_objs;
    if let Some(compression) = args.compression {
        *COMPRESSION.lock().unwrap() = lotrc::Compression::new(compression);
    }
    if let Some(unluac) = args.unluac {
        *UNLUAC.lock().unwrap() = unluac;
    }
    if args.lua_decomp && !PathBuf::from((*UNLUAC.lock().unwrap()).clone()).is_file() {
        panic!("Need to know where unluac.jar is for --lua_decomp. Add to executable directory or specify file with --unluac")
    }

    if args.command.hash {
        for input in args.input {
            let val =  Crc::from_string(&input).key();
            println!("{}: {}, 0X{:0X}", input, val, val);
        }
    } else {
        let output: PathBuf = args.output.map(|x| x.into()).unwrap_or(exe_dir);
        let mut parsed = HashSet::new();
        for input in args.input {
            parse(input, output.clone(), &args.command, &mut parsed, Some(&multi))?;
        }
    }
    Ok(())
}
