#include "ObjLoader.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(count - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), count);
    return wide;
}

std::wstring ParentPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& a, const std::string& bUtf8) {
    std::wstring b = Utf8ToWide(bUtf8);
    std::replace(b.begin(), b.end(), L'/', L'\\');
    if (a.empty()) {
        return b;
    }
    if (!b.empty() && (b[0] == L'\\' || (b.size() > 1 && b[1] == L':'))) {
        return b;
    }
    if (a.back() == L'\\' || a.back() == L'/') {
        return a + b;
    }
    return a + L'\\' + b;
}

std::ifstream OpenBinary(const std::wstring& path) {
    return std::ifstream(path, std::ios::binary);
}

// Last path-like token on an MTL map line (skips options like -bm 1.0).
std::string ParseMapPath(std::istringstream& ss) {
    std::string token;
    std::string last;
    while (ss >> token) {
        last = token;
    }
    return last;
}

struct MtlMaterial {
    std::string name;
    std::string mapKd;
    std::string mapBump;
    std::string mapDisp;
    std::string mapD;
    float ns = -1.0f;
    float pr = -1.0f;
    float pm = -1.0f;
};

std::vector<MtlMaterial> LoadMtl(const std::wstring& mtlPath) {
    std::ifstream file = OpenBinary(mtlPath);
    if (!file) {
        return {};
    }

    std::vector<MtlMaterial> materials;
    MtlMaterial current{};
    auto flush = [&]() {
        if (!current.name.empty()) {
            materials.push_back(current);
        }
        current = {};
    };

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "newmtl") {
            flush();
            ss >> current.name;
        } else if (tag == "map_Kd") {
            current.mapKd = ParseMapPath(ss);
        } else if (tag == "map_d") {
            current.mapD = ParseMapPath(ss);
        } else if (tag == "map_Bump" || tag == "bump" || tag == "norm" || tag == "map_norm") {
            current.mapBump = ParseMapPath(ss);
        } else if (tag == "map_disp" || tag == "disp") {
            current.mapDisp = ParseMapPath(ss);
        } else if (tag == "Ns") {
            ss >> current.ns;
        } else if (tag == "Pr") {
            ss >> current.pr;
        } else if (tag == "Pm") {
            ss >> current.pm;
        }
    }
    flush();
    return materials;
}

struct FaceCorner {
    int v = 0;
    int vt = 0;
    int vn = 0;
};

FaceCorner ParseCorner(const std::string& token) {
    FaceCorner c{};
    const size_t s1 = token.find('/');
    if (s1 == std::string::npos) {
        c.v = std::stoi(token);
        return c;
    }
    c.v = std::stoi(token.substr(0, s1));
    const size_t s2 = token.find('/', s1 + 1);
    if (s2 == std::string::npos) {
        if (s1 + 1 < token.size()) {
            c.vt = std::stoi(token.substr(s1 + 1));
        }
        return c;
    }
    if (s2 > s1 + 1) {
        c.vt = std::stoi(token.substr(s1 + 1, s2 - s1 - 1));
    }
    if (s2 + 1 < token.size()) {
        c.vn = std::stoi(token.substr(s2 + 1));
    }
    return c;
}

int ResolveIndex(int index, int count) {
    if (index > 0) {
        return index - 1;
    }
    if (index < 0) {
        return count + index;
    }
    return 0;
}
} // namespace

void ComputeTangents(CpuModel& model) {
    if (model.vertices.empty() || model.indices.size() < 3) {
        return;
    }

    std::vector<DirectX::XMFLOAT3> tanAccum(model.vertices.size(), {0.0f, 0.0f, 0.0f});
    std::vector<DirectX::XMFLOAT3> bitAccum(model.vertices.size(), {0.0f, 0.0f, 0.0f});

    for (size_t i = 0; i + 2 < model.indices.size(); i += 3) {
        const uint32_t i0 = model.indices[i];
        const uint32_t i1 = model.indices[i + 1];
        const uint32_t i2 = model.indices[i + 2];
        Vertex& v0 = model.vertices[i0];
        Vertex& v1 = model.vertices[i1];
        Vertex& v2 = model.vertices[i2];

        const DirectX::XMVECTOR p0 = DirectX::XMLoadFloat3(&v0.position);
        const DirectX::XMVECTOR p1 = DirectX::XMLoadFloat3(&v1.position);
        const DirectX::XMVECTOR p2 = DirectX::XMLoadFloat3(&v2.position);
        const DirectX::XMVECTOR e1 = DirectX::XMVectorSubtract(p1, p0);
        const DirectX::XMVECTOR e2 = DirectX::XMVectorSubtract(p2, p0);

        const float du1 = v1.uv.x - v0.uv.x;
        const float dv1 = v1.uv.y - v0.uv.y;
        const float du2 = v2.uv.x - v0.uv.x;
        const float dv2 = v2.uv.y - v0.uv.y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-8f) {
            continue;
        }
        const float invDet = 1.0f / det;

        DirectX::XMFLOAT3 e1f{};
        DirectX::XMFLOAT3 e2f{};
        DirectX::XMStoreFloat3(&e1f, e1);
        DirectX::XMStoreFloat3(&e2f, e2);

        const DirectX::XMFLOAT3 t{
            invDet * (dv2 * e1f.x - dv1 * e2f.x),
            invDet * (dv2 * e1f.y - dv1 * e2f.y),
            invDet * (dv2 * e1f.z - dv1 * e2f.z)};
        const DirectX::XMFLOAT3 b{
            invDet * (-du2 * e1f.x + du1 * e2f.x),
            invDet * (-du2 * e1f.y + du1 * e2f.y),
            invDet * (-du2 * e1f.z + du1 * e2f.z)};

        for (const uint32_t idx : {i0, i1, i2}) {
            tanAccum[idx].x += t.x;
            tanAccum[idx].y += t.y;
            tanAccum[idx].z += t.z;
            bitAccum[idx].x += b.x;
            bitAccum[idx].y += b.y;
            bitAccum[idx].z += b.z;
        }
    }

    for (size_t i = 0; i < model.vertices.size(); ++i) {
        Vertex& v = model.vertices[i];
        const DirectX::XMVECTOR n = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&v.normal));
        DirectX::XMVECTOR t = DirectX::XMLoadFloat3(&tanAccum[i]);
        const float tLenSq = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(t));
        if (tLenSq < 1e-8f) {
            DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            if (std::fabs(DirectX::XMVectorGetY(n)) > 0.999f) {
                up = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
            }
            t = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up, n));
        } else {
            t = DirectX::XMVector3Normalize(
                DirectX::XMVectorSubtract(t, DirectX::XMVectorScale(n, DirectX::XMVectorGetX(DirectX::XMVector3Dot(n, t)))));
        }

        const DirectX::XMVECTOR b = DirectX::XMLoadFloat3(&bitAccum[i]);
        const DirectX::XMVECTOR nCrossT = DirectX::XMVector3Cross(n, t);
        const float sign =
            DirectX::XMVectorGetX(DirectX::XMVector3Dot(nCrossT, b)) < 0.0f ? -1.0f : 1.0f;

        DirectX::XMFLOAT3 tf{};
        DirectX::XMStoreFloat3(&tf, t);
        v.tangent = {tf.x, tf.y, tf.z, sign};
    }
}

CpuModel LoadObjModel(const std::wstring& objPath) {
    std::ifstream file = OpenBinary(objPath);
    if (!file) {
        throw std::runtime_error("Failed to open OBJ file");
    }

    const std::wstring baseDir = ParentPath(objPath);
    std::vector<MtlMaterial> mtlMaterials;
    std::unordered_map<std::string, uint32_t> materialLookup;

    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT3> normals;
    std::vector<DirectX::XMFLOAT2> uvs;

    CpuModel model;
    {
        MaterialDesc def{};
        def.name = "default";
        model.materials.push_back(std::move(def));
        materialLookup["default"] = 0;
    }

    uint32_t currentMaterial = 0;
    std::string currentObject;
    SubMeshDesc currentSubmesh{};
    currentSubmesh.materialIndex = 0;

    auto flushSubmesh = [&]() {
        if (currentSubmesh.indexCount > 0) {
            currentSubmesh.objectName = currentObject;
            model.submeshes.push_back(currentSubmesh);
        }
        currentSubmesh.indexStart = static_cast<uint32_t>(model.indices.size());
        currentSubmesh.indexCount = 0;
        currentSubmesh.materialIndex = currentMaterial;
        currentSubmesh.objectName = currentObject;
    };

    auto ensureMaterial = [&](const std::string& name) -> uint32_t {
        const auto it = materialLookup.find(name);
        if (it != materialLookup.end()) {
            return it->second;
        }

        MaterialDesc desc{};
        desc.name = name;
        for (const auto& mtl : mtlMaterials) {
            if (mtl.name == name) {
                if (!mtl.mapKd.empty()) {
                    desc.diffusePath = JoinPath(baseDir, mtl.mapKd);
                }
                if (!mtl.mapBump.empty()) {
                    desc.normalPath = JoinPath(baseDir, mtl.mapBump);
                }
                if (!mtl.mapDisp.empty()) {
                    desc.displacementPath = JoinPath(baseDir, mtl.mapDisp);
                }
                desc.alphaCutout = !mtl.mapD.empty();

                // Lab 8 metallic workflow: Pr/Pm, else Ns→roughness, else defaults
                if (mtl.pr >= 0.0f) {
                    desc.roughness = std::clamp(mtl.pr, 0.04f, 1.0f);
                } else if (mtl.ns > 0.0f) {
                    desc.roughness = std::clamp(std::sqrt(2.0f / (mtl.ns + 2.0f)), 0.04f, 1.0f);
                } else {
                    desc.roughness = 0.5f;
                }
                desc.metallic = (mtl.pm >= 0.0f) ? std::clamp(mtl.pm, 0.0f, 1.0f) : 0.0f;
                desc.ao = 1.0f;
                break;
            }
        }

        const uint32_t index = static_cast<uint32_t>(model.materials.size());
        model.materials.push_back(std::move(desc));
        materialLookup.emplace(name, index);
        return index;
    };

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "mtllib") {
            std::string mtlFile;
            ss >> mtlFile;
            mtlMaterials = LoadMtl(JoinPath(baseDir, mtlFile));
        } else if (tag == "v") {
            DirectX::XMFLOAT3 p{};
            ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (tag == "vt") {
            DirectX::XMFLOAT2 t{};
            ss >> t.x >> t.y;
            t.y = 1.0f - t.y; // OBJ/Blender -> D3D
            uvs.push_back(t);
        } else if (tag == "vn") {
            DirectX::XMFLOAT3 n{};
            ss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        } else if (tag == "o") {
            flushSubmesh();
            ss >> currentObject;
            currentSubmesh.objectName = currentObject;
        } else if (tag == "usemtl") {
            std::string name;
            ss >> name;
            const uint32_t next = ensureMaterial(name);
            if (next != currentMaterial) {
                flushSubmesh();
                currentMaterial = next;
                currentSubmesh.materialIndex = currentMaterial;
            }
        } else if (tag == "f") {
            std::vector<std::string> tokens;
            std::string token;
            while (ss >> token) {
                tokens.push_back(token);
            }
            if (tokens.size() < 3) {
                continue;
            }

            auto makeVertex = [&](const FaceCorner& c) -> Vertex {
                Vertex v{};
                const int vi = ResolveIndex(c.v, static_cast<int>(positions.size()));
                v.position = positions[static_cast<size_t>(vi)];
                if (c.vn != 0 && !normals.empty()) {
                    const int ni = ResolveIndex(c.vn, static_cast<int>(normals.size()));
                    v.normal = normals[static_cast<size_t>(ni)];
                } else {
                    v.normal = {0.0f, 1.0f, 0.0f};
                }
                if (c.vt != 0 && !uvs.empty()) {
                    const int ti = ResolveIndex(c.vt, static_cast<int>(uvs.size()));
                    v.uv = uvs[static_cast<size_t>(ti)];
                }
                return v;
            };

            const FaceCorner c0 = ParseCorner(tokens[0]);
            for (size_t i = 1; i + 1 < tokens.size(); ++i) {
                const FaceCorner c1 = ParseCorner(tokens[i]);
                const FaceCorner c2 = ParseCorner(tokens[i + 1]);

                const uint32_t base = static_cast<uint32_t>(model.vertices.size());
                model.vertices.push_back(makeVertex(c0));
                model.vertices.push_back(makeVertex(c1));
                model.vertices.push_back(makeVertex(c2));
                model.indices.push_back(base);
                model.indices.push_back(base + 1);
                model.indices.push_back(base + 2);
                currentSubmesh.indexCount += 3;
            }
        }
    }

    flushSubmesh();

    if (model.vertices.empty()) {
        throw std::runtime_error("OBJ contained no geometry");
    }

    model.boundsMin = model.vertices.front().position;
    model.boundsMax = model.vertices.front().position;
    for (const auto& v : model.vertices) {
        model.boundsMin.x = std::min(model.boundsMin.x, v.position.x);
        model.boundsMin.y = std::min(model.boundsMin.y, v.position.y);
        model.boundsMin.z = std::min(model.boundsMin.z, v.position.z);
        model.boundsMax.x = std::max(model.boundsMax.x, v.position.x);
        model.boundsMax.y = std::max(model.boundsMax.y, v.position.y);
        model.boundsMax.z = std::max(model.boundsMax.z, v.position.z);
    }

    ComputeTangents(model);
    return model;
}
