#pragma once

#include "Frustum.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct OctreeItem {
    uint32_t Index = 0;
    Aabb Bounds{};
};

// Spatial octree for Lab 4 frustum queries.
class Octree {
public:
    void Build(const std::vector<OctreeItem>& items);
    void Clear();

    void QueryFrustum(
        const Frustum& frustum,
        const std::vector<OctreeItem>& items,
        uint32_t instanceCount,
        std::vector<uint32_t>& outVisible) const;

    uint32_t GetNodeCount() const { return m_nodeCount; }

private:
    struct Node {
        Aabb Bounds{};
        std::vector<uint32_t> ObjectIndices;
        std::array<std::unique_ptr<Node>, 8> Children{};

        bool IsLeaf() const {
            for (const auto& c : Children) {
                if (c) {
                    return false;
                }
            }
            return true;
        }
    };

    std::unique_ptr<Node> m_root;
    uint32_t m_nodeCount = 0;

    void Subdivide(Node& node, const std::vector<OctreeItem>& items, int depth);
};
