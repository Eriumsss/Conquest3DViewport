use pyo3::prelude::*;

pub mod audio;
pub mod bin;
pub mod level;
pub mod level_alt;
pub mod level_info;
pub mod lua_stuff;
pub mod pak;
pub mod pak_alt;
pub mod read_write;
pub mod shader;
pub mod types;

pub use flate2::Compression;

#[pymodule]
mod lotrc {
    use pyo3::prelude::*;

    #[pymodule]
    mod audio {
        #[pymodule_export]
        use crate::audio::{
            Header, Obj1, Obj2, AudioTable
        };
    }

    #[pymodule]
    mod bin {
        #[pymodule_export]
        use crate::bin::{
            Header, Radiosity, Tex, AssetHandle
        };
    }

    #[pymodule]
    mod level {
        #[pymodule_export]
        use crate::level::Level;
    }

    #[pymodule]
    mod level_alt {
        #[pymodule_export]
        use crate::level_alt::Level;
    }

    #[pymodule]
    mod level_info {
        #[pymodule_export]
        use crate::level_info::{
            Header, GamemodeVal, LevelVal, LevelInfo
        };
    }

    #[pymodule]
    mod pak_alt {
        #[pymodule_export]
        use crate::pak_alt::{
            Shape, ShapeExtra, HkShape, Animation, Mat, HkConstraint, Model, HkShapeInfo, TRS, 
            HkShape0, Box, Sphere, Capsule, Cylinder, ConvexVertices, BVTreeMesh, ConvexVerticesInfo, BVTreeMeshInfo,
            ShapeExtraInfo, Radiosity, RadiosityVal, Block, BlockHeader1, BlockHeader2, BlockValA, BlockValB,
            AnimationEvent
        };
    }

    #[pymodule]
    mod pak {
        use pyo3::prelude::*;
        #[pymodule_export]
        use crate::pak::{
            Shape, HkShape, Animation, Mat1, HkConstraint, Model, HkShapeInfo, AnimationInfo, AnimationBlockInfo,
            HkConstraintInfo, HkConstraintData, ModelInfo, Mat2, Mat3, Mat4, VBuffInfo, IBuffInfo, Header,
            ObjA, Obj0, LodMeshes, BufferInfo, MatBase, MatExtra, ShapeInfo, TextureInfo, EffectInfo, PFieldInfo, 
            GFXBlockInfo, FoliageInfo, RadiosityValsInfo, BlockAVal, VertexTypes, IndexBuffer, BoundingBox,
        };

        #[pymodule]
        mod model {
            #[pymodule_export]
            use crate::pak::model::{BlockHeader, BlockVal};
        }

        #[pymodule]
        mod shape {
            #[pymodule_export]
            use crate::pak::shape::Header;
        }

        #[pymodule]
        mod animation {
            #[pymodule_export]
            use crate::pak::animation::{
                HkaSplineSkeletalAnimationObj1Types, RotationQuantization,
                HkaSplineSkeletalAnimationObj1, HkaSplineSkeletalAnimationObj2,
                RotationPolar32, RotationThreeComp40, RotationThreeComp48,
                RotationThreeComp24, RotationStraight16, RotationUncompressed,
                HkaSplineSkeletalAnimationFlags, HkaSplineSkeletalAnimation, Obj5Header, Obj3,
            };
        }
    }
    
    #[pymodule]
    mod shader {
        #[pymodule_export]
        use crate::shader::{
            Header, ShaderHeader, Shaders
        };
    }

    #[pymodule]
    mod types {
        #[pymodule_export]
        use crate::types::{
            BaseTypes, SubBlock, Spray, GameObjs, Lua, SSA, SubBlocksHeader,
            SubBlocksBlockHeader, SSAVal, GameObjsHeader, GameObjsTypeHeader, 
            GameObjsTypeField, GameObjsObjHeader, GameObj, SprayInstance, SprayVal, 
            CrowdItem, CrowdHeader, CrowdVal, AtlasUVVal, hash_string, crc_string,
            decomp_lua, recomp_lua, unluac, compression, anim_tables, zip_, gltf
        };
    }
}
