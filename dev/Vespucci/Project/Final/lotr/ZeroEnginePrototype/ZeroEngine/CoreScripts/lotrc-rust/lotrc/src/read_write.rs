use std::io::{Read, Write};
use std::sync::{Arc, Mutex};
use std::path::{Path, PathBuf};
use std::fs;
use itertools::Itertools;
use zip::{ZipArchive, ZipWriter, write::SimpleFileOptions};
use anyhow::{Result, Context};

fn format_path(path: &Path) -> String {
    path.iter().map(|x| x.to_str().unwrap()).join("/")
}

pub trait PathStuff: Sized {
    fn path(&self) -> &Path;
    fn full_path(&self) -> PathBuf;
    fn with_path<P: AsRef<Path>>(&self, path: P) -> Self; 

    fn name(&self) -> &str {
        self.path().file_stem().unwrap().to_str().unwrap()
    }

    fn file_name(&self) -> &str {
        self.path().file_name().unwrap().to_str().unwrap()
    }

    fn with_file_name<S: AsRef<std::ffi::OsStr>>(&self, file_name: S) -> Self {
        self.with_path(self.path().with_file_name(file_name))
    }

    fn join<P: AsRef<Path>>(&self, path: P) -> Self {
        self.with_path(self.path().join(path))
    }

    fn with_extension<S: AsRef<std::ffi::OsStr>>(&self, ext: S) -> Self {
        self.with_path(self.path().with_extension(ext))
    }
}

pub enum Reader {
    File(PathBuf),
    Zip(Arc<Mutex<ZipArchive<fs::File>>>, PathBuf, PathBuf)
}

impl std::fmt::Debug for Reader {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::File(val) => val.fmt(f),
            Self::Zip(_, val, _) => val.fmt(f)
        }
    }
}

impl PathStuff for Reader {
    fn path(&self) -> &Path {
        match self {
            Self::File(path) => path.as_path(),
            Self::Zip(_, path, _) => path.as_path(),
        }
    }

    fn full_path(&self) -> PathBuf {
        match self {
            Self::File(path) => path.clone(),
            Self::Zip(_, path, base) => base.join(path),
        }
    }

    fn with_path<P: AsRef<Path>>(&self, path: P) -> Self {
        match self {
            Self::File(_) => Self::File(path.as_ref().into()),
            Self::Zip(zip, _, base) => Self::Zip(zip.clone(), path.as_ref().into(), base.clone()),
        }
    }
}

impl Reader {
    pub fn new<P: AsRef<Path>>(path: P, zip: bool) -> Result<Self> {
        Ok(if zip {
            Self::Zip(
                Arc::new(Mutex::new(ZipArchive::new(fs::File::open(path.as_ref())?)?)),
                PathBuf::new(),
                path.as_ref().into(),
            )
        } else {
            Self::File(
                path.as_ref().into()
            )
        })
    }

    pub fn is_file(&self) -> bool {
        match self {
            Self::File(path) => path.is_file(),
            Self::Zip(zip, path, _) => zip.lock().unwrap().index_for_name(&format_path(path)).is_some()
        }
    }

    pub fn read(&self) -> Result<Vec<u8>> {
        Ok(match self {
            Self::File(path) => fs::read(path).with_context(|| format!("{:?}", path.as_os_str()))?,
            Self::Zip(zip, path, _) => {
                let mut zip = zip.lock().unwrap();
                let name = format_path(path);
                let mut file = zip.by_name(&name).context(name)?;
                let mut out = Vec::with_capacity(file.size() as usize);
                file.read_to_end(&mut out)?;
                out
            }
        })
    }
}

impl IntoIterator for Reader {
    type Item = Self;
    type IntoIter = std::vec::IntoIter<Self>;
    fn into_iter(self) -> Self::IntoIter {
        let out: Vec<_> = match self {
            Self::File(path) => if path.is_dir() {
                fs::read_dir(path)
                    .unwrap()
                    .filter_map(|x| x.ok().map(|x| Self::File(x.path())))
                    .collect()
            } else {
                vec![]
            },
            Self::Zip(zip, path, base) => zip.lock().unwrap().file_names().filter_map(|x| {
                let child = PathBuf::from(x);
                child.starts_with(&path).then_some(Self::Zip(zip.clone(), child, base.clone()))
            }).collect()
        };
        out.into_iter()
    }
}

pub enum Writer {
    File(PathBuf),
    Zip(Arc<Mutex<ZipWriter<fs::File>>>, PathBuf, PathBuf)
}

impl PathStuff for Writer {
    fn path(&self) -> &Path {
        match self {
            Self::File(path) => path.as_path(),
            Self::Zip(_, path, _) => path.as_path(),
        }
    }
    fn full_path(&self) -> PathBuf {
        match self {
            Self::File(path) => path.clone(),
            Self::Zip(_, path, base) => base.join(path),
        }
    }
    fn with_path<P: AsRef<Path>>(&self, path: P) -> Self {
        match self {
            Self::File(_) => Self::File(path.as_ref().into()),
            Self::Zip(zip, _, base) => Self::Zip(zip.clone(), path.as_ref().into(), base.clone()),
        }
    }
}

impl Writer {
    pub fn new<P: AsRef<Path>>(path: P, zip: bool) -> Result<Self> {
        Ok(if zip {
            fs::create_dir_all(path.as_ref().parent().unwrap())?;
            Self::Zip(
                Arc::new(Mutex::new(ZipWriter::new(fs::File::create(path.as_ref().with_extension("zip"))?))),
                PathBuf::new(),
                path.as_ref().with_extension("zip").into()
            )
        } else {
            Self::File(
                path.as_ref().into()
            )
        })
    }

    pub fn write(&self, contents: &[u8]) -> Result<()> {
        match self {
            Self::File(path) => {
                fs::create_dir_all(path.parent().unwrap())?;
                fs::write(path, contents).with_context(|| format!("{:?}", path.as_os_str()))?;
            },
            Self::Zip(zip, path, _) => {
                let mut zip = zip.lock().unwrap();
                let name = format_path(path);
                zip.start_file(&name, SimpleFileOptions::default()).with_context(||name.clone())?;
                zip.write_all(contents).with_context(|| name.clone())?;
                zip.flush()?;
            }
        }
        Ok(())
    }
}

