#pragma once

#include "ThreatIndex.h"
#include <vector>

namespace Threats {

// QuadtreeThreatIndex — self-contained region quadtree over the threats' swept
// AABBs (no external deps; NOT supahero1's lib, which drags in a Linux-first
// allocator). Provided as a SELECTABLE broad-phase so the rollout's grid and
// quadtree backends can be A/B-compared in-game.
//
// Each threat is stored once, at the deepest node whose quadrant fully contains
// its AABB (straddlers stay at the parent); Query() recurses the nodes
// overlapping the region and rechecks exact AABB overlap. The node pool is
// reused across rebuilds (no per-frame heap churn).
//
// On Realm's dense, dynamic, small-resolution field a uniform grid generally
// wins (the per-rebuild tree build cost outweighs the query savings) — this
// backend exists so that can be measured rather than assumed.
class QuadtreeThreatIndex final : public ThreatIndex {
public:
    void Build(const std::vector<Threat>& threats) override;
    void Query(const Aabb& region, std::vector<int>& out) const override;
    const char* Name() const override { return "quad"; }

private:
    struct Node {
        Aabb             bounds;
        int              child[4];
        std::vector<int> items;
    };
    const std::vector<Threat>* m_threats   = nullptr;
    std::vector<Node>          m_nodes;        // pool, reused across builds
    int                        m_nodeCount = 0;

    int  AllocNode(const Aabb& b);
    void Insert(int nodeIdx, const Aabb& box, int idx, int depth);
    void QueryNode(int nodeIdx, const Aabb& region, std::vector<int>& out) const;
};

} // namespace Threats
