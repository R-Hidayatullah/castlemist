/* ============================================================================
 * gw2_ida_types_subsystems.h -- archive, packfile, model-file, map, video
 * Companion to gw2_ida_types.h. Every offset below was measured in the binary;
 * the function it came from is named in the comment.
 * ==========================================================================*/

/* ===================================================== Archive3 (Gw2.dat) ===
 * Arena\Services\Archive3\Archive.cpp. The runtime side of the MFT.
 * Entry point: Archive_Open @ 0x14156B590.
 */

/* Reserved MFT indices. From the asserts in Archive_AllocStreamEntry
 * (0x141566E90, "firstMftIndex >= INDEX_FIRST_FILE" against a literal 0x10)
 * and Archive_WriteMftAndDescriptor (0x14156C3A0, "INDEX_MFT <
 * m_entryArray.Count()" against a literal 3). */
enum ARCHIVE_INDEX
{
    ARCHIVE_INDEX_DESCRIPTOR  = 0,
    ARCHIVE_INDEX_FIXED       = 1,    /* IsFixedLocation(); must stay at offset 0 */
    ARCHIVE_INDEX_MFT         = 3,    /* the entry that stores the MFT's own CRC  */
    ARCHIVE_INDEX_FIRST_FILE  = 16,
};

enum ARCHIVE_ENTRY_FLAGS
{
    ARCHIVE_FLAG_ENTRY_USED   = 0x01,
    ARCHIVE_FLAG_FIRST_STREAM = 0x02,
};

/* 24 bytes. This is the same row castlemist's on-disk MftData describes, but
 * the field meanings differ from the names that struct currently uses:
 *
 *   castlemist MftData      this struct       note
 *   ------------------      -----------       ----
 *   offset                  offset            same
 *   size                    size              same
 *   compression_flag (u16)  extraBytes (u16)  NOT a compression flag
 *   entry_flag (u16)        flags(u8)+stream(u8)  two fields, not one
 *   counter                 nextStream        singly-linked stream chain
 *   crc                     crc               same
 *
 * Evidence: Archive_ReallocEntry @ 0x14156BF40 writes offset/size/extraBytes/
 * crc at +0/+8/+0xC/+0x14; Archive_AllocStreamEntry @ 0x141566E90 tests
 * flags at +0x0E, stream at +0x0F and walks nextStream at +0x10.
 */
struct ArchiveAllocEntry
{
    unsigned __int64 offset;      /* +0x00 byte offset in the archive        */
    unsigned int     size;        /* +0x08                                   */
    unsigned short   extraBytes;  /* +0x0C tail bytes beyond the payload     */
    unsigned char    flags;       /* +0x0E ARCHIVE_ENTRY_FLAGS               */
    unsigned char    stream;      /* +0x0F stream id this entry belongs to   */
    unsigned int     nextStream;  /* +0x10 next mftIndex in the chain, 0=end */
    unsigned int     crc;         /* +0x14 Crc32C over the payload           */
};

/* 72 bytes, occupying entry slots 0..2. Read by
 * Archive_WriteMftAndDescriptor @ 0x14156C3A0, which CRC32Cs these 72 bytes
 * and then continues the CRC over entries [4 .. count), deliberately skipping
 * ARCHIVE_INDEX_MFT because that is where the resulting CRC is stored. */
struct ArchiveDescriptor
{
    unsigned int signature;   /* +0x00 == 0x1A75694D                          */
    unsigned int sequence;    /* +0x04 bumped per write; skips (unsigned)-1    */
    unsigned int _unk08;
    unsigned int entryCount;  /* +0x0C == m_entryArray.Count()                 */
    unsigned int _unk10[14];  /* through +0x47                                 */
};

/* ========================================================= Packfile ("PF") ==
 * Arena\Services\Packfile\Packfile.cpp. The container every MODL / mapc /
 * AMAT / cntc asset is written in. Entry point: Packfile_Init @ 0x140DE3980.
 */
enum PACKFILE_CONST
{
    PACKFILE_SIGNATURE = 0x4650,   /* 'PF', a uint16 -- not a fourcc */
};

enum PACKFILE_HDR_FLAGS
{
    PACKFILE_HDR_BIG_ENDIAN   = 0x0001,  /* sets 0x80000000 in the runtime flags */
    PACKFILE_HDR_PTR64        = 0x0004,  /* pointer fields are 8 bytes, not 4    */
};

enum PACKFILE_CREATE_FLAGS
{
    PACKFILE_CREATE_TRANSFER_OWNERSHIP = 0x0002,  /* do not copy the buffer */
    PACKFILE_CREATE_READ_ONLY          = 0x0004,
};

struct PackfileHeader
{
    unsigned short signature;   /* +0x00 PACKFILE_SIGNATURE */
    unsigned short flags;       /* +0x02 PACKFILE_HDR_FLAGS */
    unsigned short _unk04;
    unsigned short headerSize;  /* +0x06 offset of the first chunk header */
    unsigned int   type;        /* +0x08 asset fourcc (MODL, mapc, ...)   */
};

/* Chunk lengths are measured from the end of nextChunkOffset, i.e. from +8:
 *   next   = (byte*)&chunk.version + chunk.nextChunkOffset
 *   dataSz = chunk.nextChunkOffset - chunk.headerSize + 8
 * descriptorOffset is only present when headerSize > 12. */
struct PackfileChunkHeader
{
    unsigned int   magic;            /* +0x00 chunk fourcc                */
    unsigned int   nextChunkOffset;  /* +0x04 relative to +0x08           */
    unsigned short version;          /* +0x08                             */
    unsigned short headerSize;       /* +0x0A                             */
    unsigned int   descriptorOffset; /* +0x0C only if headerSize > 12     */
};

enum PACKFILE_FLAGS
{
    PACKFILE_FLAG_STRIPPED = 0x00000001,  /* chunk descriptors removed */
};

enum PACKFILE_CHUNK_FLAGS
{
    CHUNK_FLAG_RAW_POINTERS = 0x0001,  /* offsets not yet fixed up to pointers */
};

/* ======================================================== Model file load ===
 * Arena\Engine\Model\ModelFile.cpp. The 3D model loader state machine.
 * Order: BeginLoad -> ParsePackFile -> LoadMaterials -> OnMaterialsLoaded
 *        -> Merge -> ready.
 * Values are the order the asserts appear in; only START, MATERIAL_LOADING,
 * MERGING and ERROR are directly attested, so the numbering between them is
 * inferred rather than measured. */
enum MODEL_LOAD_STAGE
{
    LOAD_STAGE_START            = 0,
    LOAD_STAGE_PARSING          = 1,
    LOAD_STAGE_MATERIAL_LOADING = 2,
    LOAD_STAGE_MERGING          = 3,
    LOAD_STAGE_READY            = 4,
    LOAD_STAGE_ERROR            = 5,
};

/* =============================================================== Scene text ==
 * Gw2\Game\Scene\Cli\ScnCliContext.cpp. What the game calls "chatter lines"
 * is the subtitle/dialogue system: ScnCli_ShowChatterLine @ 0x141476820 takes
 * a codedSpeakerName + codedText pair and a chatterLineType, looks the on-screen
 * duration up per type (ScnCli_GetChatterLineDuration @ 0x141475F40), and
 * routes it either to the speaking agent's overhead bubble or to the caption
 * widget. Asserted bound: chatterLineType <= Content::CHATTER_LINE_TYPES (201).
 * Type 7 is special-cased everywhere: a 30 s rather than 60 s suppression
 * window, and a 500 ms rather than 5000 ms re-show delay.
 * "Cinecaption" @ 0x1422E52A0 is the font style used to draw them, not the
 * system itself. */
enum SCN_CHATTER_LINE
{
    CHATTER_LINE_TYPE_SPECIAL = 7,     /* short timers; role not pinned down */
    CHATTER_LINE_TYPES        = 201,
};
