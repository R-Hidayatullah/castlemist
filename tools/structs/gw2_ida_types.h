/* ============================================================================
 * gw2_types.h  --  IDA local types for Gw2-64.exe
 *
 * Derived from:
 *   - Gw2-64.exe itself (assert strings, field offsets, table layouts)
 *   - external_source/bgfx-master   (NOTE: newer than the vendored copy, see below)
 *   - external_source/Granny-3D-SDK-main (granny 2.9.12)
 *   - external_source/bink
 *
 * GW2 engine layout (from the D:\Perforce\Live\NAEU\v2\Code\Arena\... __FILE__
 * strings baked into the binary):
 *
 *   Engine\Gr\                 renderer front end (Gr* = "graphics")
 *   Engine\Gr\Bgfx\            ArenaNet wrapper over bgfx (BgfxDdi/Draw/Texture/
 *                              Shader/Buffer/Window/UiRenderer/Utils)
 *   Engine\Gr\Bgfx\External\   VENDORED UPSTREAM bgfx + bx + bimg
 *   Engine\Gr\Img\             CPU-side image codecs (ATEX/DDS/DXT/BC7/PNG/...)
 *   Engine\Model\              model/animation runtime
 *   Engine\ModelFileFormat\    MODL chunk parsing + Granny interop
 *
 * !! VERSION WARNING !!
 * The vendored bgfx is commit a476c5b9a42d3779af59a0099d4d222fa8898d36 and is
 * OLDER than external_source/bgfx-master. Two ABI-visible differences:
 *   Attrib::Count      binary = 18 (TexCoord0..7)   master = 26 (TexCoord0..15)
 *   AttribType::Count  binary =  5 {Uint8,Uint10,Int16,Half,Float}
 *                      master =  9 {Int8,Uint8,Uint10,Int16,Uint16,Half,Float,Int32,Uint32}
 * The struct below uses the *binary's* counts. Check out that commit before
 * diffing pseudocode against bgfx-master sources.
 * ==========================================================================*/

/* ---------------------------------------------------------------- bgfx ---
 * Confirmed against the binary:
 *   BgfxVertexLayout_Add @ 0x140b5a9f0 writes m_attributes at layout+2*attrib+42
 *   and m_offset at layout+2*attrib+6, so Attrib::Count == (42-6)/2 == 18.
 *   s_attribTypeSize @ 0x141c84d18 is 5 types x 4 counts:
 *     Uint8 {1,2,4,4} Uint10 {4,4,4,4} Int16 {2,4,8,8} Half {2,4,8,8} Float {4,8,12,16}
 */
enum bgfx_RendererType
{
    BGFX_RENDERER_NOOP        = 0,
    BGFX_RENDERER_AGC         = 1,
    BGFX_RENDERER_DIRECT3D11  = 2,   /* the only backend GW2 ships */
    BGFX_RENDERER_DIRECT3D12  = 3,
    BGFX_RENDERER_GNM         = 4,
    BGFX_RENDERER_METAL       = 5,
    BGFX_RENDERER_NVN         = 6,
    BGFX_RENDERER_OPENGLES    = 7,
    BGFX_RENDERER_OPENGL      = 8,
    BGFX_RENDERER_VULKAN      = 9,
};

struct bgfx_VertexLayout
{
    unsigned int   m_hash;            /* == RendererType, indexes s_attribTypeSize */
    unsigned short m_stride;
    unsigned short m_offset[18];      /* [Attrib::Count] */
    unsigned short m_attributes[18];  /* encodeAsInt<<8 | norm<<7 | type<<3 | (num-1) */
};

/* all bgfx handles are a bare uint16; 0xFFFF == kInvalidHandle */
struct bgfx_DynamicIndexBufferHandle  { unsigned short idx; };
struct bgfx_DynamicVertexBufferHandle { unsigned short idx; };
struct bgfx_FrameBufferHandle         { unsigned short idx; };
struct bgfx_IndexBufferHandle         { unsigned short idx; };
struct bgfx_IndirectBufferHandle      { unsigned short idx; };
struct bgfx_OcclusionQueryHandle      { unsigned short idx; };
struct bgfx_ProgramHandle             { unsigned short idx; };
struct bgfx_ShaderHandle              { unsigned short idx; };
struct bgfx_TextureHandle             { unsigned short idx; };
struct bgfx_UniformHandle             { unsigned short idx; };
struct bgfx_VertexBufferHandle        { unsigned short idx; };
struct bgfx_VertexLayoutHandle        { unsigned short idx; };

struct bgfx_Memory
{
    unsigned char *data;
    unsigned int   size;
};

struct bgfx_TextureInfo
{
    unsigned int   format;      /* bgfx::TextureFormat::Enum */
    unsigned int   storageSize;
    unsigned short width;
    unsigned short height;
    unsigned short depth;
    unsigned short numLayers;
    unsigned char  numMips;
    unsigned char  bitsPerPixel;
    unsigned char  cubeMap;
};

struct bgfx_TransientIndexBuffer
{
    unsigned char *data;
    unsigned int   size;
    unsigned int   startIndex;
    unsigned short handle;      /* IndexBufferHandle */
    unsigned char  isIndex16;
};

struct bgfx_TransientVertexBuffer
{
    unsigned char           *data;
    unsigned int             size;
    unsigned int             startVertex;
    unsigned short           stride;
    unsigned short           handle;        /* VertexBufferHandle */
    unsigned short           layoutHandle;  /* VertexLayoutHandle */
};

struct bgfx_InstanceDataBuffer
{
    unsigned char *data;
    unsigned int   size;
    unsigned int   offset;
    unsigned int   num;
    unsigned short stride;
    unsigned short handle;      /* VertexBufferHandle */
};

/* ---------------------------------------------------------- GW2: GrFvf ---
 * Flexible vertex format bitmask. Ground truth:
 *   GrFvf_BuildVertexLayout @ 0x140b9c310 (bit -> bgfx attribute)
 *   GrFvf_ToString          @ 0x140ba8e90 (bit -> mnemonic letter)
 * The Add() call ORDER in GrFvf_BuildVertexLayout is the byte order in the vertex.
 */
enum GR_FVF
{
    GR_FVF_POSITION            = 0x00000001, /* 'p' 3 x float                     */
    GR_FVF_WEIGHTS             = 0x00000002, /* 'w' 4 x uint8, normalized         */
    GR_FVF_GROUP               = 0x00000004, /* 'i' 4 x uint8 raw (bone indices)  */
    GR_FVF_NORMAL              = 0x00000008, /* 'n' 3 x float                     */
    GR_FVF_COLOR               = 0x00000010, /* 'c' 4 x uint8 normalized (BGRA)   */
    GR_FVF_TANGENT             = 0x00000020, /* 't' 3 x float                     */
    GR_FVF_BITANGENT           = 0x00000040, /* 'b' 3 x float                     */
    GR_FVF_TANGENT_FRAME       = 0x00000080, /* 'f' N+T+B, each 4 x uint8 norm    */
    GR_FVF_TEXCOORD_BITS       = 0x0000FF00, /* count of float32 UV sets          */
    GR_FVF_TEXCOORD_F16_BITS   = 0x00FF0000, /* count of float16 UV sets          */
    GR_FVF_NORMAL_C            = 0x04000000, /* 4 x uint8 normalized normal       */
    GR_FVF_POSITION4           = 0x08000000, /* 4 x float position                */
    GR_FVF_POSITION_COMPRESSED = 0x10000000, /* emitted as 4 x uint8 norm normal  */
    GR_FVF_F_FLAG              = 0x20000000, /* 'F'                               */
};

/* GrFvf_BuildVertexLayout output: the cached fvf -> layout pair. */
struct GrFvfVertexLayout
{
    unsigned int             fvf;
    struct bgfx_VertexLayout layout;
};

/* -------------------------------------------------- GW2: DDI texture ---
 * Layout recovered from BgfxTexture.cpp (0x140b14fc0 .. 0x140b18290),
 * chiefly the create path at 0x140b16b20.
 */
enum DDI_TEXTURE_TYPE
{
    DDI_TEXTURE_2D   = 0,
    DDI_TEXTURE_3D   = 1,
    DDI_TEXTURE_CUBE = 2,
};

/* Bit positions confirmed by the shift/mask code:
 *   Lock2D @ 0x140b17180 tests (m_flags >> 22) & 1 and later ORs 0x400000.
 *   Create @ 0x140b16b20 ORs 0x2000000 when bgfx returns kInvalidHandle,
 *   and tests 0x800000 / 0x20 / bit15 / bit27 / bit28 while building the
 *   64-bit bgfx texture flags word.
 */
enum DDI_TEXTURE_FLAGS
{
    DDI_TEXTURE_SRGB           = 0x00000020,
    DDI_TEXTURE_FLAG_BIT15     = 0x00008000,   /* -> bgfx flag bit 44 */
    DDI_TEXTURE_LOCKED         = 0x00400000,
    DDI_TEXTURE_MSAA           = 0x00800000,   /* pulls sample count from config id 7 */
    DDI_TEXTURE_CREATE_FAILED  = 0x02000000,
    DDI_TEXTURE_FLAG_BIT27     = 0x08000000,   /* -> bgfx flag bit 46 */
    DDI_TEXTURE_RENDER_TARGET  = 0x10000000,   /* -> bgfx flag bit 47; disables mips */
    DDI_TEXTURE_FLAG_BIT31     = 0x80000000,   /* -> bgfx flag bit 43 */
};

struct DdiTextureBgfx
{
    unsigned short   m_handle;      /* +0x00 bgfx::TextureHandle, 0xFFFF = invalid */
    unsigned short   _pad02;
    unsigned int     m_type;        /* +0x04 DDI_TEXTURE_TYPE                      */
    unsigned int     _unk08;
    unsigned int     m_format;      /* +0x0C GR_FORMAT (engine side, valid 0..0x25)*/
    unsigned int     m_bgfxFormat;  /* +0x10 bgfx::TextureFormat, passed to create */
    unsigned int     m_dimsX;       /* +0x14                                       */
    unsigned int     m_dimsY;       /* +0x18                                       */
    unsigned short   m_depth;       /* +0x1C (3D)                                  */
    unsigned char    m_numMips;     /* +0x1E last create arg when mips enabled     */
    unsigned char    _pad1F;
    unsigned short   m_levels;      /* +0x20 asserted == 1 for 3D                  */
    unsigned short   _pad22;
    unsigned int     m_flags;       /* +0x24 DDI_TEXTURE_FLAGS                     */
    struct DdiTextureLock *m_lock;  /* +0x28 non-null only while locked            */
};

/* Allocated by Lock2D/Lock3D/LockCube as one block of (bytes + 16); the pixel
 * staging area is the tail, which is what m_lock->TextureDataPtr() returns. */
struct DdiTextureLock
{
    unsigned int   level;           /* +0x00 */
    unsigned int   pitch;           /* +0x04 returned through outPitch */
    unsigned int   slicePitch;      /* +0x08 returned through outSlice */
    unsigned int   bytes;           /* +0x0C */
    /* +0x10 : pixel staging bytes[] -- TextureDataPtr() */
};

/* ------------------------------------------------ GW2: ATEX / Img ---
 * ImgAtex_IsAtexMagic @ 0x140b89290 masks the fourcc with 0xFFFFFFF9 and
 * accepts the six variants below.
 */
enum ATEX_MAGIC
{
    ATEX_MAGIC_ATEX = 0x58455441,   /* plain                        */
    ATEX_MAGIC_ATTX = 0x58545441,   /* terrain                      */
    ATEX_MAGIC_ATEC = 0x43455441,
    ATEX_MAGIC_ATEP = 0x50455441,
    ATEX_MAGIC_ATEU = 0x55455441,
    ATEX_MAGIC_ATET = 0x54455441,
};

struct AtexHeader
{
    unsigned int   magic;           /* ATEX_MAGIC_*                             */
    unsigned int   format;          /* fourcc: DXT1/DXT3/DXT5/DXTA/DXTL/DXTN/3DCX */
    unsigned short width;
    unsigned short height;
    /* mip levels follow, each: uint32 byteCount then RLE/DXT payload;
       see ImgAtex_Decode @ 0x140b86b70 and ImgAtexCommon_DecodeRleBlocks
       @ 0x140c213f0. */
};

/* ---------------------------------------- GW2: AMAT 'BGFX' shader chunk ---
 * Loaded by BgfxShader_LoadFromAmat @ 0x140bfcff0 via
 *   GrMat_GetShaderChunk(shaderId, 0x58464742 = 'BGFX', version = 3, &techCount)
 * Field offsets come from the assert strings in BgfxShader.cpp
 * (0x140bfdc40 = the technique/pass/effect walk).
 */
struct AmatTechnique
{
    unsigned int   nameToken;       /* +0x00 Token_Decode32 -> "l"/"m"/"h"/"u"  */
    unsigned int   passCount;       /* +0x04                                    */
    void          *passes;          /* +0x08 AmatPass[]                         */
};

struct AmatBgfxData
{
    unsigned int          _unk00[6];
    unsigned int          techniqueCount;   /* +0x18 */
    struct AmatTechnique *techniques;       /* +0x1C, 16 bytes per entry */
    /* shaderCount / shader table live further in; referenced by the asserts
       "vertexShader->vertexShaderIndex < m_data->shaderCount" and
       "effectData->pixelShaderIndex < m_data->shaderCount". */
};

enum GR_SHADER_QUALITY
{
    GR_SHADER_QUALITY_INVALID = 0,
    GR_SHADER_QUALITY_LOW     = 1,   /* technique token 'l' */
    GR_SHADER_QUALITY_MEDIUM  = 2,   /* 'm' */
    GR_SHADER_QUALITY_HIGH    = 3,   /* 'h' */
    GR_SHADER_QUALITY_ULTRA   = 4,   /* 'u' */
    GR_SHADER_QUALITIES       = 5,   /* asserted never to be stored */
};

enum GR_SHADER_TECHNIQUE_FLAGS
{
    GR_SHADER_TECH_INVALID_NAME = 0x01,
    GR_SHADER_TECH_HAS_LOW      = 0x02,
    GR_SHADER_TECH_HAS_MEDIUM   = 0x04,
    GR_SHADER_TECH_HAS_HIGH     = 0x08,
    GR_SHADER_TECH_HAS_ULTRA    = 0x10,
};

struct BgfxShader
{
    struct AmatBgfxData *m_data;            /* +0x00 */
    unsigned int         m_techniqueCount;  /* +0x08 */
    unsigned int         m_shaderId;        /* +0x0C */
    unsigned char        m_techniqueIndex;  /* +0x10 0xFF = none selected */
    unsigned char        m_techniqueFlags;  /* +0x11 GR_SHADER_TECHNIQUE_FLAGS */
    unsigned short       _pad12;
    unsigned int         m_quality;         /* +0x14 GR_SHADER_QUALITY */
    unsigned short       m_memCategory;     /* +0x18 */
    unsigned short       m_memCategory2;    /* +0x1A */
    void                *_unk20;
    void                *m_passes;          /* +0x20 */
    void                *_unk28;
    unsigned int         m_passCount;       /* +0x2C asserted != 0 */
    unsigned char        m_lock[48];        /* +0x30 two 24-byte rw locks */
};
