#ifndef GW2_CLOTH_SIM_H
#define GW2_CLOTH_SIM_H

// cloth_sim.h - bridge between castlemist's model mesh (ModelMeshCPU / GVertex)
// and the reverse-engineered GW2 cloth solver in GrCloth.h. It turns a
// live mesh into a simulated cloth, steps it, and writes the deformed positions
// + rebuilt normals back into the vertex array for the renderer to re-upload.
//
// The GW2 MODL struct template does not (yet) define the ModelClothData chunk,
// so a model's real per-cloth pin/constraint data cannot be pulled from file
// here. Instead the bridge auto-pins the top band of the mesh (GW2 is Z-up) and
// simulates the whole mesh as cloth using the exact solver + its real
// area-weighted / index-buffer normal rebuild. When cloth-chunk extraction is
// added, feed loadModelClothData()'s ClothLoadResult into buildFromClothData().

#include <cstdint>
#include <vector>

#include "castlemist/extract/model_types.h"  // GVertex, ModelMeshCPU
#include "castlemist/sim/GrCloth.h"          // the solver itself

namespace castlemist::cloth {

/// @brief Turns a live mesh into a simulated cloth and writes the result back.
class ClothSim {
public:
    /// Build a cloth from a mesh. Pins the band of verts within `pinFraction` of
    /// the maximum extent along `pinAxis` (0=X,1=Y,2=Z; Z-up => 2 pins the top),
    /// so the sheet hangs from that edge. Returns false if the mesh is unusable
    /// (too few verts, or no verts fall in the pin band).
    bool buildFromMesh(const ModelMeshCPU& mesh, int pinAxis = 2, float pinFraction = 0.12f);

    /// Build from real file cloth data (once ModelClothData extraction exists):
    /// `rest` are the mesh rest positions (from geometry), the rest come straight
    /// from loadModelClothData(). numLocked verts must already be at the front.
    bool buildFromClothData(const std::vector<GVertex>&        meshVerts,
                            const std::vector<uint32_t>&       meshIndices,
                            uint32_t                           numLocked,
                            std::vector<castlemist::cloth::ClothConstraint>  constraints,
                            std::vector<castlemist::cloth::ClothWeld>        welds,
                            std::vector<castlemist::cloth::ClothObstacle>    obstacles,
                            const castlemist::cloth::ClothParams&            params);

    /// Build from authored file cloth pieces (ModelPreview::clothPieces): combines
    /// all pieces into one solver with every piece's pinned prefix collected at the
    /// front, using the real file constraints. Also assembles a renderable proxy
    /// mesh (proxyVertices/proxyIndices + per-material draw ranges). Returns false
    /// if there are no usable pieces.
    bool buildFromPieces(const std::vector<ClothPieceCPU>& pieces);
    bool fileDriven() const { return m_fileDriven; }

    /// The simulated proxy mesh, for the renderer. proxyVertices() is refreshed by
    /// writeBackProxy() (call after update()); proxyIndices()/proxyRanges() are
    /// static. Each range draws a contiguous span of proxy triangles with one
    /// material (materialIndex into the model's materials).
    struct DrawRange { uint32_t materialIndex, indexStart, indexCount; };
    const std::vector<GVertex>&    proxyVertices() const { return m_proxy; }
    const std::vector<uint32_t>&   proxyIndices()  const { return m_proxyIdx; }
    const std::vector<DrawRange>&  proxyRanges()   const { return m_ranges; }
    const std::vector<uint32_t>&   clothMaterials() const { return m_clothMats; }
    void writeBackProxy();   // update m_proxy pos+normals from the current sim state

    bool active() const { return m_active; }
    void clear();

    /// Advance the simulation. `windWorld` is an optional world-space wind vector
    /// (nullptr = none). Locked verts stay pinned in place (no skeleton binding in
    /// the demo path). Call once per rendered frame.
    void update(float dt, const float windWorld[3] = nullptr);

    /// Copy the current simulated positions + normals back into `verts` (indexed
    /// in the mesh's original vertex order). `verts` must be the same mesh used to
    /// build; only px/py/pz and nx/ny/nz are touched.
    void writeBack(std::vector<GVertex>& verts) const;

    uint32_t vertexCount() const { return m_fileDriven ? (uint32_t)m_proxy.size() : (uint32_t)m_toMesh.size(); }
    uint32_t lockedCount() const { return m_numLocked; }

    /// Displacement of the simulated verts from their rest pose (for verification).
    void displacementStats(float& maxDisp, float& meanDisp) const;

private:
    /// Reorders mesh verts so the `pinned` set comes first (GrCloth needs locked
    /// verts as a front prefix), records the remap, and builds edge constraints
    /// from the triangle list. Shared by both build paths.
    bool buildReordered(const ModelMeshCPU& mesh, const std::vector<char>& pinned);

    castlemist::cloth::GrCloth          m_cloth;
    std::vector<uint32_t> m_toMesh;   // cloth vert index -> original mesh vert index
    uint32_t              m_numLocked = 0;
    bool                  m_active = false;

    /// file-driven proxy mesh (buildFromPieces)
    bool                       m_fileDriven = false;
    std::vector<GVertex>       m_proxy;      // proxy verts in solver (locked-first) order
    std::vector<uint32_t>      m_proxyIdx;   // proxy triangle indices (solver order)
    std::vector<DrawRange>     m_ranges;     // per-piece material draw ranges
    std::vector<uint32_t>      m_clothMats;  // distinct material indices the cloth covers
};

} // namespace castlemist::cloth

#endif // GW2_CLOTH_SIM_H
