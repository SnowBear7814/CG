#include "Octree.h"

#include <algorithm>

using namespace DirectX;

namespace {
constexpr int kMaxObjectsPerLeaf = 8;
constexpr int kMaxDepth = 10;

bool AabbOverlaps(const Aabb& a, const Aabb& b) {
    if (!a.IsValid() || !b.IsValid()) {
        return false;
    }
    return a.Min.x <= b.Max.x && a.Max.x >= b.Min.x && a.Min.y <= b.Max.y && a.Max.y >= b.Min.y &&
           a.Min.z <= b.Max.z && a.Max.z >= b.Min.z;
}

Aabb ChildBounds(const Aabb& parent, int octant) {
    const float cx = (parent.Min.x + parent.Max.x) * 0.5f;
    const float cy = (parent.Min.y + parent.Max.y) * 0.5f;
    const float cz = (parent.Min.z + parent.Max.z) * 0.5f;

    const int ox = octant & 1;
    const int oy = (octant >> 1) & 1;
    const int oz = (octant >> 2) & 1;

    Aabb child = parent;
    if (ox == 0) {
        child.Max.x = cx;
    } else {
        child.Min.x = cx;
    }
    if (oy == 0) {
        child.Max.y = cy;
    } else {
        child.Min.y = cy;
    }
    if (oz == 0) {
        child.Max.z = cz;
    } else {
        child.Min.z = cz;
    }
    return child;
}

Aabb BoundsFromItems(const std::vector<OctreeItem>& items, const std::vector<uint32_t>& indices) {
    Aabb b{};
    for (uint32_t idx : indices) {
        b.Merge(items[idx].Bounds);
    }
    return b;
}
} // namespace

void Octree::Clear() {
    m_root.reset();
    m_nodeCount = 0;
}

void Octree::Build(const std::vector<OctreeItem>& items) {
    Clear();
    if (items.empty()) {
        return;
    }

    m_root = std::make_unique<Node>();
    m_root->ObjectIndices.reserve(items.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(items.size()); ++i) {
        m_root->ObjectIndices.push_back(i);
    }
    m_root->Bounds = BoundsFromItems(items, m_root->ObjectIndices);
    m_nodeCount = 1;

    Subdivide(*m_root, items, 0);
}

void Octree::Subdivide(Node& node, const std::vector<OctreeItem>& items, int depth) {
    if (static_cast<int>(node.ObjectIndices.size()) <= kMaxObjectsPerLeaf || depth >= kMaxDepth) {
        return;
    }

    std::array<std::vector<uint32_t>, 8> buckets;
    for (uint32_t objIdx : node.ObjectIndices) {
        const Aabb& ob = items[objIdx].Bounds;
        for (int o = 0; o < 8; ++o) {
            if (AabbOverlaps(ob, ChildBounds(node.Bounds, o))) {
                buckets[static_cast<size_t>(o)].push_back(objIdx);
            }
        }
    }

    node.ObjectIndices.clear();

    for (int o = 0; o < 8; ++o) {
        std::vector<uint32_t>& bucket = buckets[static_cast<size_t>(o)];
        if (bucket.empty()) {
            continue;
        }

        auto child = std::make_unique<Node>();
        child->ObjectIndices = std::move(bucket);
        child->Bounds = BoundsFromItems(items, child->ObjectIndices);
        ++m_nodeCount;
        Subdivide(*child, items, depth + 1);
        node.Children[static_cast<size_t>(o)] = std::move(child);
    }
}

void Octree::QueryFrustum(
    const Frustum& frustum,
    const std::vector<OctreeItem>& items,
    uint32_t instanceCount,
    std::vector<uint32_t>& outVisible) const {
    outVisible.clear();
    if (!m_root || items.empty()) {
        return;
    }

    std::vector<uint8_t> seen(instanceCount, 0);
    outVisible.reserve(items.size());

    std::vector<const Node*> stack;
    stack.push_back(m_root.get());

    while (!stack.empty()) {
        const Node* node = stack.back();
        stack.pop_back();
        if (!node || !frustum.IntersectsAabb(node->Bounds)) {
            continue;
        }

        if (node->IsLeaf()) {
            for (uint32_t itemIdx : node->ObjectIndices) {
                if (itemIdx >= items.size()) {
                    continue;
                }

                const uint32_t inst = items[itemIdx].Index;
                if (inst >= instanceCount || seen[inst]) {
                    continue;
                }

                if (!frustum.IntersectsAabb(items[itemIdx].Bounds)) {
                    continue;
                }

                seen[inst] = 1;
                outVisible.push_back(inst);
            }
            continue;
        }

        for (int o = 7; o >= 0; --o) {
            if (node->Children[static_cast<size_t>(o)]) {
                stack.push_back(node->Children[static_cast<size_t>(o)].get());
            }
        }
    }
}
