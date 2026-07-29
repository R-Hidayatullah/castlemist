/// @file
/// @brief Tests for the cloth-solver bridge.
///
/// The solver itself is reverse-engineered from the game and lives in
/// minieditor/GrCloth. What is checked here is the bridge: which vertices get
/// pinned, that the sheet actually moves, and that the pinned prefix never
/// does. A cloth whose anchors drift is the failure mode that looks like the
/// whole model falling through the floor.

#include "test_framework.h"

#include "castlemist/sim/cloth_sim.h"

#include <cmath>
#include <vector>

namespace {

/// @brief Build a flat grid of vertices in the XY plane at a range of Z heights.
///
/// GW2 is Z-up, so the highest-Z band is the one the bridge pins.
ModelMeshCPU make_sheet(int nx = 8, int nz = 8) {
    ModelMeshCPU m;
    for (int z = 0; z < nz; ++z) {
        for (int x = 0; x < nx; ++x) {
            GVertex v{};
            v.px = float(x);
            v.py = 0.0f;
            v.pz = float(z);
            v.nz = 1.0f;
            m.vertices.push_back(v);
        }
    }
    for (int z = 0; z + 1 < nz; ++z) {
        for (int x = 0; x + 1 < nx; ++x) {
            uint32_t a = uint32_t(z * nx + x), b = a + 1;
            uint32_t c = uint32_t((z + 1) * nx + x), d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    m.vertexCount = uint32_t(m.vertices.size());
    return m;
}

float max_z(const std::vector<GVertex>& v) {
    float m = -1e30f;
    for (const GVertex& g : v) m = g.pz > m ? g.pz : m;
    return m;
}

} // namespace

CM_TEST(cloth, refuses_a_mesh_with_nothing_to_simulate) {
    castlemist::cloth::ClothSim sim;
    ModelMeshCPU empty;
    CHECK_FALSE(sim.buildFromMesh(empty));
    CHECK_FALSE(sim.active());
    CHECK_EQ(sim.vertexCount(), 0u);

    ModelMeshCPU tiny;
    tiny.vertices.resize(2);
    tiny.vertexCount = 2;
    CHECK_FALSE(sim.buildFromMesh(tiny));
}

CM_TEST(cloth, builds_from_a_sheet_and_pins_the_top_band) {
    castlemist::cloth::ClothSim sim;
    ModelMeshCPU sheet = make_sheet();
    CHECK(sim.buildFromMesh(sheet, /*pinAxis=*/2, /*pinFraction=*/0.2f));
    CHECK(sim.active());
    CHECK_FALSE(sim.fileDriven());
}

// A mesh with no extent along the pin axis has no "top" to hang from, so there
// is no anchor band and the build must fail rather than free-fall.
CM_TEST(cloth, refuses_a_mesh_that_is_flat_along_the_pin_axis) {
    castlemist::cloth::ClothSim sim;
    ModelMeshCPU sheet = make_sheet();
    for (GVertex& v : sheet.vertices) v.pz = 0.0f;   // every vertex at the same height
    CHECK_FALSE(sim.buildFromMesh(sheet, 2, 0.2f));
    CHECK_FALSE(sim.active());
}

// pinFraction is clamped to [0.01, 0.9], so out-of-range values still produce a
// usable cloth instead of failing or pinning everything.
CM_TEST(cloth, clamps_an_out_of_range_pin_fraction) {
    ModelMeshCPU sheet = make_sheet();

    castlemist::cloth::ClothSim low;
    CHECK(low.buildFromMesh(sheet, 2, -1.0f));   // clamped up to 0.01
    CHECK(low.active());

    castlemist::cloth::ClothSim high;
    CHECK(high.buildFromMesh(sheet, 2, 5.0f));   // clamped down to 0.9
    CHECK(high.active());

    // The wider band pins strictly more vertices.
    CHECK(high.lockedCount() > low.lockedCount());
}

// An invalid axis falls back to Z (GW2 is Z-up), rather than reading past the end.
CM_TEST(cloth, an_out_of_range_pin_axis_falls_back_to_z) {
    ModelMeshCPU sheet = make_sheet();
    castlemist::cloth::ClothSim a, b;
    CHECK(a.buildFromMesh(sheet, 2, 0.2f));
    CHECK(b.buildFromMesh(sheet, 99, 0.2f));
    CHECK_EQ(a.lockedCount(), b.lockedCount());
}

CM_TEST(cloth, an_empty_piece_list_is_not_file_driven) {
    castlemist::cloth::ClothSim sim;
    std::vector<ClothPieceCPU> none;
    CHECK_FALSE(sim.buildFromPieces(none));
    CHECK_FALSE(sim.fileDriven());
}

CM_TEST(cloth, file_pieces_drive_the_solver_and_expose_a_proxy) {
    ClothPieceCPU piece;
    ModelMeshCPU sheet = make_sheet(6, 6);
    piece.vertices = sheet.vertices;
    piece.indices = sheet.indices;
    piece.materialIndex = 3;
    piece.lockCount = 6; // the first row is pinned
    for (uint32_t i = 0; i + 1 < piece.vertices.size(); ++i) {
        ClothEdgeCPU e;
        e.a = uint16_t(i);
        e.b = uint16_t(i + 1);
        e.rest = 1.0f;
        piece.edges.push_back(e);
    }

    castlemist::cloth::ClothSim sim;
    std::vector<ClothPieceCPU> pieces{piece};
    if (!sim.buildFromPieces(pieces)) SKIP("solver rejected the synthetic piece");

    CHECK(sim.fileDriven());
    CHECK(sim.active());
    CHECK_EQ(sim.proxyVertices().size(), piece.vertices.size());
    CHECK_EQ(sim.proxyIndices().size(), piece.indices.size());
    CHECK_FALSE(sim.proxyRanges().empty());
    CHECK_EQ(sim.proxyRanges()[0].materialIndex, 3u);
    CHECK_EQ(sim.clothMaterials().size(), size_t{1});
    CHECK_EQ(sim.clothMaterials()[0], 3u);
    CHECK_EQ(sim.lockedCount(), 6u);
}

CM_TEST(cloth, the_pinned_band_never_moves_under_gravity) {
    castlemist::cloth::ClothSim sim;
    ModelMeshCPU sheet = make_sheet(8, 8);
    if (!sim.buildFromMesh(sheet, 2, 0.2f)) SKIP("solver rejected the synthetic sheet");

    const float pinned_z = max_z(sheet.vertices);

    std::vector<GVertex> verts = sheet.vertices;
    for (int step = 0; step < 60; ++step) sim.update(1.0f / 60.0f);
    sim.writeBack(verts);

    // The anchors stay exactly where they were; nothing rises above them.
    CHECK_NEAR(max_z(verts), pinned_z, 1e-3);

    // ...and every position stayed finite (a NaN here silently blanks the mesh).
    for (const GVertex& v : verts) {
        CHECK(v.px == v.px);
        CHECK(v.py == v.py);
        CHECK(v.pz == v.pz);
    }
}

CM_TEST(cloth, wind_displaces_the_free_vertices) {
    ModelMeshCPU sheet = make_sheet(8, 8);

    castlemist::cloth::ClothSim calm;
    if (!calm.buildFromMesh(sheet, 2, 0.2f)) SKIP("solver rejected the synthetic sheet");
    std::vector<GVertex> a = sheet.vertices;
    for (int i = 0; i < 60; ++i) calm.update(1.0f / 60.0f);
    calm.writeBack(a);

    castlemist::cloth::ClothSim windy;
    CHECK(windy.buildFromMesh(sheet, 2, 0.2f));
    const float wind[3] = {400.0f, 0.0f, 0.0f};
    std::vector<GVertex> b = sheet.vertices;
    for (int i = 0; i < 60; ++i) windy.update(1.0f / 60.0f, wind);
    windy.writeBack(b);

    float drift = 0;
    for (size_t i = 0; i < a.size(); ++i) drift += std::fabs(a[i].px - b[i].px);
    CHECK(drift > 0.0f);
}
