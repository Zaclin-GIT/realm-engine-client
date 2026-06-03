#include "pch-il2cpp.h"
#include "QuadtreeThreatIndex.h"

#include <algorithm>

namespace Threats {

namespace {
constexpr int kMaxDepth = 8;   // subdivision cap (deep enough for tile-scale items)
}

int QuadtreeThreatIndex::AllocNode(const Aabb& b)
{
    const int i = m_nodeCount++;
    if (i >= static_cast<int>(m_nodes.size())) m_nodes.resize(i + 1);
    Node& n = m_nodes[i];
    n.bounds = b;
    n.child[0] = n.child[1] = n.child[2] = n.child[3] = -1;
    n.items.clear();           // keeps capacity across rebuilds
    return i;
}

// Descend from nodeIdx, placing idx in the deepest node whose quadrant fully
// contains box. Iterative so we never hold a Node& across AllocNode (which may
// reallocate the pool and invalidate references).
void QuadtreeThreatIndex::Insert(int nodeIdx, const Aabb& box, int idx, int depth)
{
    for (;;) {
        if (depth >= kMaxDepth) { m_nodes[nodeIdx].items.push_back(idx); return; }
        const Aabb  b    = m_nodes[nodeIdx].bounds;     // copy — no dangling ref
        const float midX = 0.5f * (b.minX + b.maxX);
        const float midY = 0.5f * (b.minY + b.maxY);
        const bool  left   = box.maxX <= midX;
        const bool  right  = box.minX >= midX;
        const bool  bottom = box.maxY <= midY;
        const bool  top    = box.minY >= midY;
        if (!((left || right) && (bottom || top))) {    // straddles a split line
            m_nodes[nodeIdx].items.push_back(idx);
            return;
        }
        const int qx = right ? 1 : 0;
        const int qy = top   ? 1 : 0;
        const int q  = qy * 2 + qx;
        int c = m_nodes[nodeIdx].child[q];
        if (c == -1) {
            Aabb cb;
            cb.minX = qx ? midX : b.minX; cb.maxX = qx ? b.maxX : midX;
            cb.minY = qy ? midY : b.minY; cb.maxY = qy ? b.maxY : midY;
            c = AllocNode(cb);                          // may reallocate m_nodes
            m_nodes[nodeIdx].child[q] = c;              // re-index after alloc
        }
        nodeIdx = c; ++depth;                           // descend
    }
}

void QuadtreeThreatIndex::Build(const std::vector<Threat>& threats)
{
    m_threats = &threats;
    m_nodeCount = 0;
    const int n = static_cast<int>(threats.size());
    if (n == 0) return;

    float minX = threats[0].box.minX, minY = threats[0].box.minY;
    float maxX = threats[0].box.maxX, maxY = threats[0].box.maxY;
    for (int i = 1; i < n; ++i) {
        const Aabb& b = threats[i].box;
        minX = std::min(minX, b.minX); minY = std::min(minY, b.minY);
        maxX = std::max(maxX, b.maxX); maxY = std::max(maxY, b.maxY);
    }
    // Guard a degenerate (zero-area) root so split midpoints stay finite.
    if (maxX <= minX) maxX = minX + 1.f;
    if (maxY <= minY) maxY = minY + 1.f;

    const int root = AllocNode(Aabb{ minX, minY, maxX, maxY });
    for (int i = 0; i < n; ++i)
        Insert(root, threats[i].box, i, 0);
}

void QuadtreeThreatIndex::QueryNode(int nodeIdx, const Aabb& region,
                                    std::vector<int>& out) const
{
    const Node& n = m_nodes[nodeIdx];     // safe: Query never mutates the pool
    if (!AabbOverlap(n.bounds, region)) return;
    const std::vector<Threat>& th = *m_threats;
    for (int idx : n.items)
        if (AabbOverlap(th[idx].box, region)) out.push_back(idx);
    for (int q = 0; q < 4; ++q)
        if (n.child[q] != -1) QueryNode(n.child[q], region, out);
}

void QuadtreeThreatIndex::Query(const Aabb& region, std::vector<int>& out) const
{
    if (m_nodeCount == 0 || !m_threats) return;
    QueryNode(0, region, out);
}

} // namespace Threats
