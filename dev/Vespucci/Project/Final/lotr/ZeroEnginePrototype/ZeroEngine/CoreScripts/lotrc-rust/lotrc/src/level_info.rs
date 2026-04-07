use std::{fs, path::Path};
use log::warn;
use serde::{Serialize, Deserialize};
use lotrc_proc::{OrderedData, basicpymethods, PyMethods};
use anyhow::{Result, Context};
use pyo3::prelude::*;
use super::{
    types::{self, Crc, OrderedData, Version, PC, XBOX, from_bytes, dump_bytes, AsData},
    read_write::{Reader, Writer, PathStuff},
};

#[derive(Debug, Default, Clone, Serialize, Deserialize)]
pub struct Name(String);

impl <'py> IntoPyObject<'py> for Name {
    type Target = <String as IntoPyObject<'py>>::Target;
    type Output = <String as IntoPyObject<'py>>::Output;
    type Error = <String as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.0.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Name {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self(String::extract_bound(ob)?))
    }
}

impl From<Name> for [u8; 32] {
    fn from(value: Name) -> Self {
        let mut val = [0u8; 32];
        let n = value.0.len().min(32);
        val[..n].copy_from_slice(&value.0.as_bytes()[..n]);
        val
    }
}

impl From<[u8; 32]> for Name {
    fn from(value: [u8; 32]) -> Self {
        let mut i = 0;
        while i < 32 {
            if value[i] == 0 {
                break;
            }
            i += 1
        }
        Name(String::from_utf8(value[..i].to_vec()).unwrap())
    }
}

impl OrderedData for Name {
    type PC = [u8; 32];
    type XBOX = [u8; 32];
    type PS3 = [u8; 32];
}

#[basicpymethods]
#[pyclass(module="level_info", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Header {
    pub constx04: u32,
    pub dlc: u32,
    pub strings_offset: u32,
    pub strings_size: u32,
    pub strings_num: u32,
    pub string_keys_size: u32,
    pub string_keys_offset: u32,
    pub locale_strings_size: u32,
    pub locale_strings_offset: u32,
    pub gamemodes_num: u32,
    pub gamemodes_offset: u32,
    pub levels_num: u32,
    pub levels_offset: u32,
    pub size2048: u32,
}

#[basicpymethods]
#[pyclass(module="level_info", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct LevelVal {
    pub name: Name,
    pub key_name: Crc,
    pub key_description: Crc,
    pub dlc: u32,
    pub gamemodes: u32,
}

#[basicpymethods]
#[pyclass(module="level_info", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct GamemodeVal {
    pub key: Crc,
    pub key_name: Crc,
    pub key_description: Crc,
}

use serde_with::serde_as;
#[pyclass(module="level_info", set_all, get_all)]
#[serde_as]
#[derive(Default, Debug, Serialize, Deserialize, PyMethods)]
pub struct LevelInfo {
    header: Header,
    #[serde(skip)]
    strings: types::Strings,
    #[serde(skip)]
    string_keys: types::StringKeys,
    #[serde(skip)]
    locale_strings: types::SubBlocks,
    levels: Vec<LevelVal>,
    gamemodes: Vec<GamemodeVal>,
    #[serde_as(as = "serde_with::hex::Hex")]
    extra: Vec<u8>
}

#[basicpymethods(no_bytes)]
#[pymethods]
impl LevelInfo {
    #[staticmethod]
    fn load(path: String) -> Result<Self> {
        let path = std::path::PathBuf::from(path);
        if path.with_extension("zip").is_file() {
            Self::from_file(Reader::new(path, true)?)
        } else if path.is_dir() {
            Self::from_file(Reader::new(path, false)?)
        } else {
            Self::parse(path)
        }
    }

    fn dump_files(&self, path: String, zip: bool) -> Result<()> {
        self.to_file(Writer::new(path, zip)?)
    }

    fn dump_pc(&self, path: String) -> Result<()> {
        self.dump::<PC, _>(path)
    }
}

impl LevelInfo {
    pub fn parse<P: AsRef<Path>>(path: P) -> Result<Self> {
        let data = fs::read(path.as_ref()).context(path.as_ref().display().to_string())?;
        if data[0] == 4 {
            Self::from_data::<PC>(&data[..])
        } else if data[3] == 4 {
            Self::from_data::<XBOX>(&data[..])
        } else {
            warn!("Invalid level_info data");
            Ok(Default::default())
        }
    }

    pub fn dump<O: Version + 'static, P: AsRef<Path>>(&self, path: P) -> Result<()> {
        let path = path.as_ref().with_extension("dat");
        path.parent().map(fs::create_dir_all);
        fs::write(&path, self.to_data::<O>()).context(path.display().to_string())?;
        Ok(())
    }

    pub fn from_data<O: Version + 'static>(data: &[u8]) -> Result<Self> {
        let header: Header = from_bytes!(O, data)?;
        let strings = from_bytes!(O, types::Strings, &data[header.strings_offset as usize..], header.strings_num as usize)?;
        types::update_strings(&strings.strings);
        let string_keys = from_bytes!(O, types::StringKeys, &data[header.string_keys_offset as usize..])?;
        let locale_strings = from_bytes!(O, types::SubBlocks, &data[header.locale_strings_offset as usize..], None.into())?;
        let gamemodes = from_bytes!(O, &data[header.gamemodes_offset as usize..], header.gamemodes_num as usize)?;
        let levels = from_bytes!(O, &data[header.levels_offset as usize..], header.levels_num as usize)?;
        let extra = data[0x38..0x13c].to_vec();

        Ok(Self {
            header,
            strings,
            string_keys,
            locale_strings,
            gamemodes,
            levels,
            extra
        })
    }

    pub fn to_data<O: Version + 'static>(&self) -> Vec<u8> {
        let mut header = self.header.clone();
        header.gamemodes_offset = 0x13c;
        header.gamemodes_num = self.gamemodes.len() as u32;
        header.levels_offset = header.gamemodes_offset + header.gamemodes_num * O::size::<GamemodeVal>() as u32;
        header.levels_num = self.levels.len() as u32;
        header.string_keys_offset = header.levels_offset + header.levels_num * O::size::<LevelVal>() as u32;
        header.string_keys_size = self.string_keys.size::<O>() as u32;
        header.locale_strings_offset = header.string_keys_offset + header.string_keys_size;
        header.locale_strings_size = self.locale_strings.size::<O>() as u32;
        header.strings_offset = header.locale_strings_offset + header.locale_strings_size;
        header.strings_size = self.strings.size::<O>() as u32;
        header.strings_num = self.strings.len() as u32;
        header.size2048 = (header.strings_offset + 2047) & 0xFFFFF800;

        let mut data = dump_bytes!(O, header);
        data.extend(self.extra.clone());
        data.extend(dump_bytes!(O, self.gamemodes));
        data.extend(dump_bytes!(O, self.levels));
        data.extend(dump_bytes!(O, self.string_keys));
        data.extend(dump_bytes!(O, self.locale_strings, None.into()));
        data.extend(dump_bytes!(O, self.strings));
        data
    }

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.join("index.json").write(&serde_json::to_vec_pretty(self)?)?;
        self.strings.to_file(writer.join("debug_strings"))?;
        self.string_keys.to_file(writer.join("string_keys"))?;
        self.locale_strings.to_file(writer.join("locale_strings"), &self.string_keys, None)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        let mut val = serde_json::from_slice::<Self>(&reader.join("index.json").read()?).context("index.json")?;
        val.strings = types::Strings::from_file(reader.join("debug_strings"))?;
        val.string_keys = types::StringKeys::from_file(reader.join("string_keys"))?;
        val.locale_strings = types::SubBlocks::from_file(reader.join("locale_strings"), None)?;
        Ok(val)
    }
}
