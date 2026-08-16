/* ============================================================================
 * gw2_types_granny.h -- Granny / Bink / compression types for Gw2-64.exe
 * Companion to gw2_types.h. Parse gw2_types.h first.
 *
 * GRANNY 2.9.12, statically linked. Evidence: the "..\source\granny_*.cpp"
 * __FILE__ strings at 0x142100d70+ (granny_mesh.cpp, granny_skeleton.cpp,
 * granny_local_pose.cpp, granny_track_mask.cpp, granny_file_format.cpp, ...)
 * and "Switchable types no longer supported in Granny 2.7+".
 *
 * !! Granny structs in this build are PACKED -- no inter-member padding. !!
 * Proof: ModelAnimSubControl_BindUvTransformTracks @ 0x140d4ddf0 reads
 *   group->VectorTrackCount at +8   and
 *   group->VectorTracks     at +12  (a QWORD at a 4-aligned offset).
 * With natural x64 alignment VectorTracks would sit at +16. Everything below
 * is therefore wrapped in pack(1). Matching asserts in the binary:
 *   "trackGroup < ModelUtilGetGrannyAnim(&data)->TrackGroupCount"
 *   "trackIndex < group->VectorTrackCount"
 * ==========================================================================*/

#pragma pack(push, 1)

struct granny_variant { void *Type; void *Object; };
struct granny_curve2  { struct granny_variant CurveData; };

struct granny_transform
{
    unsigned int Flags;
    float        Position[3];
    float        Orientation[4];
    float        ScaleShear[9];
};

struct granny_data_type_definition
{
    int    Type;            /* granny_member_type */
    char  *Name;
    struct granny_data_type_definition *ReferenceType;
    int    ArrayWidth;
    int    Extra[3];
    unsigned __int64 Ignored__;
};

struct granny_vertex_data
{
    struct granny_data_type_definition *VertexType;
    int             VertexCount;
    unsigned char  *Vertices;
    int             VertexComponentNameCount;
    char          **VertexComponentNames;
    int             VertexAnnotationSetCount;
    void           *VertexAnnotationSets;
};

struct granny_tri_material_group { int MaterialIndex; int TriFirst; int TriCount; };

struct granny_tri_topology
{
    int   GroupCount;
    struct granny_tri_material_group *Groups;
    int   IndexCount;
    int  *Indices;
    int   Index16Count;
    unsigned short *Indices16;
    int   VertexToVertexCount;
    int  *VertexToVertexMap;
    int   VertexToTriangleCount;
    int  *VertexToTriangleMap;
    int   SideToNeighborCount;
    unsigned int *SideToNeighborMap;
    int   BonesForTriangleCount;
    int  *BonesForTriangle;
    int   TriangleToBoneCount;
    int  *TriangleToBoneIndices;
    int   TriAnnotationSetCount;
    void *TriAnnotationSets;
};

struct granny_bone_binding
{
    char  *BoneName;
    float  OBBMin[3];
    float  OBBMax[3];
    int    TriangleCount;
    int   *TriangleIndices;
};

struct granny_mesh
{
    char  *Name;
    struct granny_vertex_data  *PrimaryVertexData;
    int    MorphTargetCount;
    void  *MorphTargets;
    struct granny_tri_topology *PrimaryTopology;
    int    MaterialBindingCount;
    void  *MaterialBindings;
    int    BoneBindingCount;
    struct granny_bone_binding *BoneBindings;
    struct granny_variant ExtendedData;
};

struct granny_bone
{
    char  *Name;
    int    ParentIndex;
    struct granny_transform LocalTransform;
    float  InverseWorld4x4[16];
    float  LODError;
    struct granny_variant ExtendedData;
};

struct granny_skeleton
{
    char *Name;
    int   BoneCount;
    struct granny_bone *Bones;
    int   LODType;
    struct granny_variant ExtendedData;
};

struct granny_model
{
    char  *Name;
    struct granny_skeleton *Skeleton;
    struct granny_transform InitialPlacement;
    int    MeshBindingCount;
    void  *MeshBindings;          /* granny_model_mesh_binding[] == granny_mesh*[] */
    struct granny_variant ExtendedData;
};

struct granny_vector_track
{
    char        *Name;
    unsigned int TrackKey;
    int          Dimension;
    struct granny_curve2 ValueCurve;   /* stride confirmed as 32 in the binary */
};

struct granny_transform_track
{
    char *Name;
    int   Flags;
    struct granny_curve2 OrientationCurve;
    struct granny_curve2 PositionCurve;
    struct granny_curve2 ScaleShearCurve;
};

struct granny_track_group
{
    char *Name;                                    /* +0x00 */
    int   VectorTrackCount;                        /* +0x08 */
    struct granny_vector_track    *VectorTracks;   /* +0x0C  <- packed */
    int   TransformTrackCount;
    struct granny_transform_track *TransformTracks;
    int   TransformLODErrorCount;
    float *TransformLODErrors;
    int   TextTrackCount;
    void *TextTracks;
    struct granny_transform InitialPlacement;
    int   Flags;
    float LoopTranslation[3];
    void *PeriodicLoop;
    struct granny_variant ExtendedData;
};

struct granny_animation
{
    char *Name;                                    /* +0x00 */
    float Duration;                                /* +0x08 */
    float TimeStep;                                /* +0x0C */
    float Oversampling;                            /* +0x10 */
    int   TrackGroupCount;                         /* +0x14 */
    struct granny_track_group **TrackGroups;       /* +0x18 */
    int   DefaultLoopCount;
    int   Flags;
    struct granny_variant ExtendedData;
};

#pragma pack(pop)

/* ---------------------------------------------------------------------------
 * Granny vertex attribute name -> GR_FVF bit.
 * Table: s_grannyAttribToFvf @ 0x141ee7cc0, 28 entries x 24 bytes.
 * Walked by ModelFileFormatGranny_VertexTypeToFvf @ 0x140d556b0, which
 * strcmp()s every granny_data_type_definition member Name against it and ORs
 * the fvf bits together. Fatal on an unknown name ("Unknown granny vertex
 * attribute - %s"); asserts at most 16 elements (MAX_GRANNY_VERTEX_ELEMENTS).
 *
 *  name                       fvfBit      grannyMemberType       comps
 *  Position                   0x00000001  10 Real32Member          3
 *  Position                   0x10000000  21 Real16Member          3   compressed
 *  BoneWeights                0x00000002  14 NormalUInt8Member     4
 *  BoneIndices                0x00000004  12 UInt8Member           4
 *  Normal                     0x00000008  10 Real32Member          3
 *  Normal                     0x04000000  19 Int32Member           1   packed
 *  DiffuseColor               0x00000010  11 Int8Member            4
 *  DiffuseColor0              0x00000010  11 Int8Member            4
 *  Tangent                    0x00000020  10 Real32Member          3
 *  Binormal                   0x00000040  10 Real32Member          3
 *  TangentFrame               0x00000080  19 Int32Member           3
 *  TangentFrame               0x20000000  19 Int32Member           3
 *  TextureCoordinates0..7     0x100<<n    10 Real32Member          2
 *  TextureCoordinatesF16_0..7 0x10000<<n  21 Real16Member          2
 * ------------------------------------------------------------------------- */
struct GrannyAttribToFvf
{
    char        *grannyName;
    unsigned int fvfBit;
    unsigned int grannyMemberType;
    unsigned int componentCount;
    unsigned int _pad;
};

/* ------------------------------------------------------------------ Bink 2 --
 * bink2w64.dll is loaded at runtime by Bink_LoadDll @ 0x140d7b0a0. 22 entry
 * points are resolved by name from the length-prefixed blob at 0x1425454da
 * through the pointer table at 0x1425452b8:
 *   BinkGetError BinkOpen BinkGetFrameBuffersInfo BinkRegisterFrameBuffers
 *   BinkNextFrame BinkWait BinkClose BinkPause BinkGoto BinkSetVolume
 *   BinkShouldSkip BinkStartAsyncThread BinkDoFrameAsync BinkDoFrameAsyncWait
 *   BinkRequestStopAsyncThread BinkWaitStopAsyncThread BinkGetSummary
 *   BinkSetSoundTrack BinkSetIO BinkSetSoundSystem BinkOpenDirectSound
 *   BinkSetMemory
 * Allocations are aligned to BINK2_MEM_ALIGNMENT. Use
 * external_source/bink/bink2/bink.h for BINK / BINKFRAMEBUFFERS.
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------- ArenaNet Cmp codec ----
 * Gw2.dat block compression is ArenaNet's own Huffman codec, not zlib:
 *   Cmp_Decompress                     @ 0x140d9d9f0   <- entry point
 *   Cmp_Compress                       @ 0x140d9d7b0
 *   Cmp_DecompressMethod0              @ 0x140da27f0
 *   Cmp_CompressMethod0                @ 0x140da1220
 *   Cmp_DecompressMethod1              @ 0x140da0650
 *   Cmp_CompressMethod1                @ 0x140d9e970
 *   CmpHuff_ComputeCodeLengthHistogram @ 0x140da9da0
 * Texture payloads are compressed separately by the codecs in Engine\Gr\Img:
 *   ImgAtex_Decode                @ 0x140b86b70
 *   ImgAtexCommon_DecodeRleBlocks @ 0x140c213f0
 *   ImgDxt_DecodeBlocksSse        @ 0x140bfa2e0
 *   ImgBc7_CompressBlocks         @ 0x140c16430
 * -------------------------------------------------------------------------- */
