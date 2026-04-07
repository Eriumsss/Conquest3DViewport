use super::*;

// scalar quantization
// Bits8
// Bits16
//

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum HkaSplineSkeletalAnimationObj1Types {
    Empty(),
    Type1(Vec<u8>),
    Type2(Vec<u16>),
    Type3(Vec<u16>),
    Type4(Vec<u16>),
}

impl Default for HkaSplineSkeletalAnimationObj1Types {
    fn default() -> Self { 
        Self::Empty()
    }
}

impl AsData<'_, '_> for HkaSplineSkeletalAnimationObj1Types {
    type InArgs = (usize, u8);
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], (num, kind): Self::InArgs) -> Result<Self> {
        Ok(match kind {
            0 =>  Self::Type1(from_bytes!(V, &data[..], num)?),
            1 =>  Self::Type2(from_bytes!(V, &data[..], num)?),
            2 =>  Self::Type3(from_bytes!(V, &data[..], num)?),
            3 =>  Self::Type4(from_bytes!(V, &data[..], num)?),
            _ => panic!("Illegal Type for spline thingy")
        })
    }
    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        
        match self {
            Self::Type1(vals) => dump_bytes!(V, vals),
            Self::Type2(vals) => dump_bytes!(V, vals),
            Self::Type3(vals) => dump_bytes!(V, vals),
            Self::Type4(vals) => dump_bytes!(V, vals),
            _ => vec![]
        }
    }
    fn size<V: Version>(&self) -> usize {
        match self {
            Self::Type1(vals) => vals.size::<V>(),
            Self::Type2(vals) => vals.size::<V>(),
            Self::Type3(vals) => vals.size::<V>(),
            Self::Type4(vals) => vals.size::<V>(),
            _ => 0,
        }
    }
}

impl HkaSplineSkeletalAnimationObj1Types {
    pub fn empty(kind: u8) -> Self {
        match kind {
            0 =>  Self::Type1(vec![]),
            1 =>  Self::Type2(vec![]),
            2 =>  Self::Type3(vec![]),
            3 =>  Self::Type4(vec![]),
            _ => panic!("Illegal Type for spline thingy")
        }
    }

    pub fn kind(&self) -> u8 {
        match self {
            Self::Type1(_) => 0,
            Self::Type2(_) => 1,
            Self::Type3(_) => 2,
            Self::Type4(_) => 3,
            _ => panic!("Illegal Type for spline thingy")
        }
    }
}

#[basicpymethods]
#[pyclass(module="pak.animation", set_all, get_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct HkaSplineSkeletalAnimationObj1 {
    pub nbytes: usize,
    pub flags: u8,
    pub s1: u16,
    pub s2: u8,
    pub data: Vec<u8>,
    pub vals_a: Vec<f32>,
    pub vals: HkaSplineSkeletalAnimationObj1Types,
}

impl HkaSplineSkeletalAnimationObj1 {
    const COUNTS: [usize; 8] = [0,1,1,2,1,2,2,3];
}

impl AsData<'_, '_> for HkaSplineSkeletalAnimationObj1 {
    type InArgs = (u8, u8);
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], (flags, kind): Self::InArgs) -> Result<Self> {
        let mut val = Self::default();
        let mut offset = 0;
        val.flags = flags;
        val.vals = HkaSplineSkeletalAnimationObj1Types::empty(kind);
        if flags == 0 { return Ok(val); }
        if flags & 0xf0 != 0 {
            val.s1 = from_bytes!(V, &data[offset..])?;
            offset += V::size::<u16>();
            val.s2 = from_bytes!(V, &data[offset..])?;
            offset += V::size::<u8>();
            val.data = from_bytes!(V, &data[offset..], val.s1 as usize + val.s2 as usize + 2)?;
            offset += val.data.size::<V>();
        }

        offset = (offset + 3) & 0xfffffffc;
        let num = Self::COUNTS[(flags & 7) as usize] + 2 * Self::COUNTS[(((flags >> 4) & !flags) & 7) as usize];
        val.vals_a = from_bytes!(V, &data[offset..], num)?;
        offset += val.vals_a.size::<V>();

        if flags & 0xf0 == 0 {
            val.nbytes = offset;
            return Ok(val);
        }

        offset = (offset + 1) & 0xfffffffe;

        let num = Self::COUNTS[((flags >> 4) & 7) as usize] * (val.s1 as usize + 1);
        val.vals = from_bytes!(V, &data[offset..], num, kind)?;
        offset += val.vals.size::<V>();
        val.nbytes = offset;
        Ok(val)

    }
    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let mut data = vec![];
        if self.flags == 0 { return data; }
        if self.flags & 0xf0 != 0 {
            data.extend(dump_bytes!(V, self.s1));
            data.extend(dump_bytes!(V, self.s2));
            data.extend(dump_bytes!(V, self.data));
        }
        let off = (data.len() + 3) & 0xfffffffc;
        data.extend(vec![0u8; off-data.len()]);

        data.extend(dump_bytes!(V, self.vals_a));

        if self.flags & 0xf0 == 0 { return data; }

        let off = (data.len() + 1) & 0xfffffffe;
        data.extend(vec![0u8; off-data.len()]);
        data.extend(dump_bytes!(V, self.vals));
        data
    }
    fn size<V: Version>(&self) -> usize {
        self.nbytes
    }
}

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct RotationPolar32 { a: u32 }

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
// should be (u8, u8, u8, u16) but for xbox conv it is (u8, u8, u8, u8, u8)
pub struct RotationThreeComp40 { a: u8, b: u8, c: u8, d: u8, e: u8 }

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct RotationThreeComp48 { a: u16, b: u16, c: u16 }

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct RotationThreeComp24 { a: u8, b: u8, c: u8}

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct RotationStraight16 { a: u8, b: u8 }

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct RotationUncompressed { a: f32, b: f32, c: f32, d: f32 }

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum RotationQuantization {
    Polar32(Vec<RotationPolar32>),
    ThreeComp40(Vec<RotationThreeComp40>),
    ThreeComp48(Vec<RotationThreeComp48>),
    ThreeComp24(Vec<RotationThreeComp24>),
    Straight16(Vec<RotationStraight16>),
    Uncompressed(Vec<RotationUncompressed>),
    Empty()
}

impl Default for RotationQuantization {
    fn default() -> Self { 
        Self::Empty()
    }
}

impl AsData<'_, '_> for RotationQuantization {
    type InArgs = (usize, u8);
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], (num, kind): Self::InArgs) -> Result<Self> {
        Ok(match kind {
            0 =>  Self::Polar32(from_bytes!(V, &data[..], num)?),
            1 =>  Self::ThreeComp40(from_bytes!(V, &data[..], num)?),
            2 =>  Self::ThreeComp48(from_bytes!(V, &data[..], num)?),
            3 =>  Self::ThreeComp24(from_bytes!(V, &data[..], num)?),
            4 =>  Self::Straight16(from_bytes!(V, &data[..], num)?),
            5 =>  Self::Uncompressed(from_bytes!(V, &data[..], num)?),
            _ => panic!("Invalid Rotation Compression method")
        })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        match self {
            Self::Polar32(vals) => dump_bytes!(V, vals),
            Self::ThreeComp40(vals) => dump_bytes!(V, vals),
            Self::ThreeComp48(vals) => dump_bytes!(V, vals),
            Self::ThreeComp24(vals) => dump_bytes!(V, vals),
            Self::Straight16(vals) => dump_bytes!(V, vals),
            Self::Uncompressed(vals) => dump_bytes!(V, vals),
            _ => vec![]
        }
    }

    fn size<V: Version>(&self) -> usize {
        match self {
            Self::Polar32(vals) => vals.size::<V>(),
            Self::ThreeComp40(vals) => vals.size::<V>(),
            Self::ThreeComp48(vals) => vals.size::<V>(),
            Self::ThreeComp24(vals) => vals.size::<V>(),
            Self::Straight16(vals) => vals.size::<V>(),
            Self::Uncompressed(vals) => vals.size::<V>(),
            _ => 0
        }
    }
}

impl RotationQuantization {
    pub fn empty(kind: u8) -> Self {
        match kind {
            0 =>  Self::Polar32(vec![]),
            1 =>  Self::ThreeComp40(vec![]),
            2 =>  Self::ThreeComp48(vec![]),
            3 =>  Self::ThreeComp24(vec![]),
            4 =>  Self::Straight16(vec![]),
            5 =>  Self::Uncompressed(vec![]),
            _ => Self::Empty()
        }
    }

    pub fn kind(&self) -> u8 {
        match self {
            Self::Polar32(_) => 0,
            Self::ThreeComp40(_) => 1,
            Self::ThreeComp48(_) => 2,
            Self::ThreeComp24(_) => 3,
            Self::Straight16(_) => 4,
            Self::Uncompressed(_) => 5,
            _ => panic!("Invalid Type for Rotation Quantization")
        }
    }

    pub fn align(&self) -> u32 {
        match self {
            Self::Polar32(_) => 4,
            Self::ThreeComp40(_) => 1,
            Self::ThreeComp48(_) => 2,
            Self::ThreeComp24(_) => 1,
            Self::Straight16(_) => 2,
            Self::Uncompressed(_) => 4,
            _ => 0
        }
    }
}

#[basicpymethods]
#[pyclass(module="pak.animation", set_all, get_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct HkaSplineSkeletalAnimationObj2 {
    pub nbytes: usize,
    pub flags: u8,
    pub s1: u16,
    pub s2: u8,
    pub data: Vec<u8>,
    pub vals: RotationQuantization,
}

impl AsData<'_, '_> for HkaSplineSkeletalAnimationObj2 {
    type InArgs = (u8, u8);
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], (flags, kind): Self::InArgs) -> Result<Self> {
        let mut val = Self::default();
        let mut offset = 0;
        val.flags = flags;
        val.vals = RotationQuantization::empty(kind);
        if flags != 0 {
            if flags & 0xf0 == 0 {
                val.s1 = 0;
                val.s2 = 0;
            } else {
                val.s1 = from_bytes!(V, &data[offset..])?;
                offset += V::size::<u16>();
                val.s2 = from_bytes!(V, &data[offset..])?;
                offset += V::size::<u8>();
                val.data = from_bytes!(V, &data[offset..], val.s1 as usize + val.s2 as usize + 2)?;
                offset += val.data.size::<V>();
            }

            offset = ((offset as u32 + val.vals.align() - 1) & !(val.vals.align() - 1)) as usize;
            val.vals = from_bytes!(V, &data[offset..], val.s1 as usize + 1, kind)?;
            offset += val.vals.size::<V>();   
        }
        val.nbytes = offset;
        Ok(val)
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let mut data = vec![];
        if self.flags == 0 { return data; }
        if self.flags & 0xf0 != 0 {
            data.extend(dump_bytes!(V, self.s1));
            data.extend(dump_bytes!(V, self.s2));
            data.extend(dump_bytes!(V, self.data));
        }
        let off = ((data.len() as u32 + self.vals.align() - 1) & !(self.vals.align() - 1)) as usize;
        data.extend(vec![0u8; off-data.len()]);
        data.extend(dump_bytes!(V, self.vals));
        data
    }

    fn size<V: Version>(&self) -> usize {
        self.nbytes
    }
}

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct HkaSplineSkeletalAnimationFlags{
    pub f: u8, 
    pub a: u8, 
    pub b: u8, 
    pub c: u8,
}

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct HkaSplineSkeletalAnimation {
    pub block_starts: Vec<u32>,
    pub block_starts2: Vec<u32>,
    pub obj_c3: Vec<u32>,
    pub obj_c4: Vec<u32>,
    pub vals_a: Vec<Vec<HkaSplineSkeletalAnimationObj1>>,
    pub vals_b: Vec<Vec<HkaSplineSkeletalAnimationObj2>>,
    pub vals_c: Vec<Vec<HkaSplineSkeletalAnimationObj1>>,
    pub vals_d: Vec<Vec<HkaSplineSkeletalAnimationObj1>>,
}

impl <'a, 'b> AsData<'a, 'b> for HkaSplineSkeletalAnimation {
    type InArgs = &'a AnimationInfo;
    type OutArgs = &'b AnimationInfo;

    fn from_bytes<V: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let mut val = Self::default();
        val.block_starts = from_bytes!(V, &data[info.block_starts_offset as usize..], info.block_starts_num as usize)?;
        val.block_starts2 = from_bytes!(V, &data[info.block_starts2_offset as usize..], info.block_starts2_num as usize)?;
        val.obj_c3 = from_bytes!(V, &data[info.obj_c3_offset as usize..], info.obj_c3_num as usize)?;
        val.obj_c4 = from_bytes!(V, &data[info.obj_c4_offset as usize..], info.obj_c4_num as usize)?;
        for (start, start2) in zip(&val.block_starts,&val.block_starts2) {
            let off = (start + info.block_offset) as usize;
            let flags: Vec<HkaSplineSkeletalAnimationFlags> = from_bytes!(V, &data[off..], info.vals_num as usize)?;
            let flags2: Vec<u8> = from_bytes!(V, &data[off + flags.size::<V>()..], info.vals2_num as usize)?;
            let mut off = (info.block_offset + start + info.data_offset) as usize;
            let mut vals_a = Vec::with_capacity(flags.len());
            let mut vals_b = Vec::with_capacity(flags.len());
            let mut vals_c = Vec::with_capacity(flags.len());
            let mut vals_d = Vec::with_capacity(flags2.len());
            for flag in &flags {
                let a = from_bytes!(V, HkaSplineSkeletalAnimationObj1, &data[off..], flag.a, flag.f & 3)?;
                off += a.size::<V>();
                off = (off + 3) & 0xfffffffc;
                let b = from_bytes!(V, HkaSplineSkeletalAnimationObj2, &data[off..], flag.b, (flag.f >> 2) & 0xf)?;
                off += b.size::<V>();
                off = (off + 3) & 0xfffffffc;
                let c = from_bytes!(V, HkaSplineSkeletalAnimationObj1, &data[off..], flag.c, (flag.f >> 6) & 3)?;
                off += c.size::<V>();
                off = (off + 3) & 0xfffffffc;
                vals_a.push(a);
                vals_b.push(b);
                vals_c.push(c);
            }
            off = (info.block_offset + start + start2) as usize;
            for flag in &flags2 {
                let d = from_bytes!(V, HkaSplineSkeletalAnimationObj1, &data[off..], flag & 0xf9, (flag >> 1) & 3)?;
                off += d.size::<V>();
                off = (off + 3) & 0xfffffffc;
                vals_d.push(d);
            }
            val.vals_a.push(vals_a);
            val.vals_b.push(vals_b);
            val.vals_c.push(vals_c);
            val.vals_d.push(vals_d);
        }
        Ok(val)
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }

    fn to_bytes<V: Version>(&self, data: &mut [u8], info: Self::OutArgs) -> Result<()> {
        to_bytes!(V, self.block_starts, &mut data[info.block_starts_offset as usize..])?;
        to_bytes!(V, self.block_starts2, &mut data[info.block_starts2_offset as usize..])?;
        to_bytes!(V, self.obj_c3, &mut data[info.obj_c3_offset as usize..])?;
        to_bytes!(V, self.obj_c4, &mut data[info.obj_c4_offset as usize..])?;
        for ((start, start2), ((vals_a, vals_b), (vals_c, vals_d))) in zip(zip(&self.block_starts, &self.block_starts2), zip(zip(&self.vals_a, &self.vals_b), zip(&self.vals_c, &self.vals_d))) {
            let flags: Vec<_> = zip(vals_a, zip(vals_b, vals_c)).map(|(a, (b,c))| {
                let f = a.vals.kind() | (b.vals.kind() << 2) | (c.vals.kind() << 6);
                HkaSplineSkeletalAnimationFlags { a: a.flags, b: b.flags, c: c.flags, f }
            }).collect();
            let flags2: Vec<_> = vals_d.iter().map(|d| d.flags | (d.vals.kind() << 1)).collect();
            to_bytes!(V, flags, &mut data[(start + info.block_offset) as usize..])?;
            to_bytes!(V, flags2, &mut data[(start + info.block_offset) as usize + flags.size::<V>()..])?;
            let mut off = (info.block_offset + start + info.data_offset) as usize;
            for (a, (b, c)) in zip(vals_a, zip(vals_b, vals_c)) {
                to_bytes!(V, a, &mut data[off..])?;
                off += a.size::<V>();
                off = (off + 3) & 0xfffffffc;
                to_bytes!(V, b, &mut data[off..])?;
                off += b.size::<V>();
                off = (off + 3) & 0xfffffffc;
                to_bytes!(V, c, &mut data[off..])?;
                off += c.size::<V>();
                off = (off + 3) & 0xfffffffc;
            }
            off = (info.block_offset + start + start2) as usize;
            for d in vals_d {
                to_bytes!(V, d, &mut data[off..])?;
                off += d.size::<V>();
                off = (off + 3) & 0xfffffffc;
            }
        }
        Ok(())
    }
}

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Obj5Header {
    pub obj_a_num: u32,
    pub obj_a_offset: u32,
    pub obj_b_num: u32,
    pub obj_b_offset: u32,
}

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Obj5Val {
    pub unk_0: f32,
    pub unk_1: f32,
    pub unk_2: f32,
    pub unk_3: f32,
    pub unk_4: f32,
    pub unk_5: f32,
    pub unk_6: f32,
}

#[basicpymethods]
#[pyclass(module="pak.animation", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Obj3 {
    pub t: f32,
    pub event: Crc,
    pub dat_2: u32,
    pub dat_3: u32,
    pub dat_4: u32,
    pub dat_5: u32,
    pub dat_6: u32,
    pub dat_7: u32,
    pub dat_8: u32,
    pub dat_9: u32,
    pub dat_10: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct AnimationInfo {
    pub key: Crc,
    pub gamemodemask: i32,
    pub offset: u32,
    pub size: u32,
    pub kind: u32, // always 3
    pub unk_5: f32,
    pub vals_num: u32,
    pub vals2_num: u32,
    pub unk_8: u32,
    pub vala: u32, // numFrames
    pub unk_10: u32, // numBlocks
    pub unk_11: u32, // maxFramesPerBlock
    pub data_offset: u32, // relative to block_starts, maskAndQuantizationSize
    pub unk_13: f32, // blockDuration
    pub unk_14: f32, // blockInverseDuration
    pub t_scale: f32, // frameDuration
    pub block_starts_offset: u32, //relative to block_offset, blockOffsets
    pub block_starts_num: u32,  // relative to block_starts, nunBlocks
    pub block_starts2_offset: u32, // floatBlockOffsets
    pub block_starts2_num: u32, // numFloatBlocks
    pub obj_c3_offset: u32, // unused, equal to block_start, transformOffsets
    pub obj_c3_num: u32, // unused, equal to block_start, numTransforms
    pub obj_c4_offset: u32, // floatOffsets
    pub obj_c4_num: u32, // numFloats
    pub block_offset: u32, // data
    pub block_size: u32, // dataSize
    pub obj3_num: u32, // numEvents
    pub obj3_offset: u32, // eventOffsets
    pub bones_num1: u32, // bones is at least this + obj1_num long
    pub unk_29: u32,
    pub obj1_num: u32,
    pub bones_offset: u32,
    pub unk_32: u32,
    pub obj1_offset: u32,
    pub obj2_offset: u32,
    pub obj2_num: u32,
    pub obj5_offset: u32, // to some object that contains offsets in pos 1 and 2 and a value in pos 0
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct AnimationBlockInfo {
    pub key: Crc,
    #[serde(alias="unk_1")]
    pub guid: u32,
    pub key_name: Crc,
    pub offset: u32,
    pub size: u32,
    pub size_comp: u32,
    pub unk_6: u32,
    pub unk_7: u32,
    pub unk_8: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct Animation {
    pub obj1: Vec<u32>,
    pub obj2: Vec<u32>,
    pub obj3: Vec<Obj3>,
    pub bones: Vec<Crc>,
    pub obj5_header: Option<Obj5Header>,
    pub obj5_a: Vec<Obj5Val>,
    pub obj5_b: Vec<Obj5Val>,
    pub obj_c: Option<animation::HkaSplineSkeletalAnimation>,
}

impl <'a, 'b> AsData<'a, 'b> for Animation {
    type InArgs = &'a AnimationInfo;
    type OutArgs = &'b AnimationInfo;

    fn from_bytes<V: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let obj1 = from_bytes!(V, &data[info.obj1_offset as usize..], info.obj1_num as usize * 2)?;
        let obj2 = from_bytes!(V, &data[info.obj2_offset as usize..], info.obj2_num as usize * 4)?;
        let obj3 = from_bytes!(V, &data[info.obj3_offset as usize..], info.obj3_num as usize)?;
        let bones = from_bytes!(V, &data[info.bones_offset as usize..], (info.vals_num + info.obj1_num) as usize)?;
        let (obj5_header, obj5_a, obj5_b) = if info.obj5_offset != 0 {
            let obj5_header: animation::Obj5Header = from_bytes!(V, &data[info.obj5_offset as usize..])?;
            let obj5_a = from_bytes!(V, &data[obj5_header.obj_a_offset as usize..], obj5_header.obj_a_num as usize)?;
            let obj5_b = from_bytes!(V, &data[obj5_header.obj_b_offset as usize..], obj5_header.obj_b_num as usize)?;
            (Some(obj5_header), obj5_a, obj5_b)
        } else {
            (None, vec![], vec![])
        };
        let obj_c = if info.kind == 3 {
            Some(from_bytes!(V, HkaSplineSkeletalAnimation, &data[..], info)?)
        } else if info.kind < 3 {
            warn!("Unhandled animation type {}", info.kind);
            None
        } else {
            warn!("Unknown animation type {}", info.kind);
            None
        };
        Ok(Self { obj1, obj2, obj3, bones, obj5_header, obj5_a, obj5_b, obj_c })
        
    }

    fn to_bytes<V: Version>(&self, data: &mut [u8], info: Self::OutArgs) -> Result<()> {
        to_bytes!(V, self.obj1, &mut data[info.obj1_offset as usize..])?;
        to_bytes!(V, self.obj2, &mut data[info.obj2_offset as usize..])?;
        to_bytes!(V, self.obj3, &mut data[info.obj3_offset as usize..])?;
        to_bytes!(V, self.bones, &mut data[info.bones_offset as usize..])?;
        if let Some(obj5_header) = &self.obj5_header {
            to_bytes!(V, obj5_header, &mut data[info.obj5_offset as usize..])?;
            to_bytes!(V, self.obj5_a, &mut data[obj5_header.obj_a_offset as usize..])?;
            to_bytes!(V, self.obj5_b, &mut data[obj5_header.obj_b_offset as usize..])?;
        }
        if let Some(obj_c) = &self.obj_c {
            to_bytes!(V, obj_c, &mut data[..], info)?;
        }
        Ok(())

    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}

impl Animation {
    pub fn unpack_block<O: Version + 'static>(anims: &mut [HashMap<usize, Self>], infos: &[AnimationInfo], data: & [u8], offset: usize, index: usize) -> Result<()> {
        let mut offset = offset;
        for (anim, info) in zip(anims, infos) {
            let gamemodemask = 1i32 << index;
            if gamemodemask & info.gamemodemask != 0 {
                anim.insert(index, from_bytes!(O, &data[offset..], info)?); 
                offset += info.size as usize;
            }
        }
        Ok(())
    }

    pub fn pack_block<O: Version + 'static>(anims: & [HashMap<usize, Self>], infos: &[AnimationInfo], data: &mut [u8], offset: usize, index: usize) -> Result<()> {
        let mut offset = offset;
        for (anim, info) in zip(anims, infos) {
            let gamemodemask = 1i32 << index;
            if gamemodemask & info.gamemodemask != 0 {
                to_bytes!(O, anim.get(&index).unwrap(), &mut data[offset..], info)?;
                offset += info.size as usize;
            }
        }
        Ok(())
    }
}
