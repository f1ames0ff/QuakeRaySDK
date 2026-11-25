// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "EditorDtalPipeline.h"

#include "EditorLightSelection.h"
#include "EditorOverlayPipeline.h"
#include "EditorSpatial.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace
{

constexpr int kMaxEmissivePolysPerFace = 64;
constexpr float kDtalPi = 3.14159265358979323846f;

std::string LowerAscii(const std::string &text)
{
    std::string out = text;
    for (char &ch : out)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

float Dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float Length3(const float v[3])
{
    return std::sqrt(Dot3(v, v));
}

bool Normalize3(float v[3])
{
    const float len = Length3(v);
    if (!(len > 1e-9f))
    {
        return false;
    }
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return true;
}

int ClipUvPolyEdge(const QrFloat2D *in, int n, int axis, float edge, bool keepGreater,
                   QrFloat2D *out)
{
    int m = 0;
    for (int i = 0; i < n; i++)
    {
        const QrFloat2D a = in[i];
        const QrFloat2D b = in[(i + 1) % n];
        const float da = keepGreater ? (a.data[axis] - edge) : (edge - a.data[axis]);
        const float db = keepGreater ? (b.data[axis] - edge) : (edge - b.data[axis]);
        const bool ia = da >= 0.0f;
        const bool ib = db >= 0.0f;

        if (ia)
        {
            if (m >= MAX_TEXTURED_AREA_LIGHT_VERTS)
            {
                return 0;
            }
            out[m++] = a;
        }
        if (ia != ib)
        {
            const float t = da / (da - db);
            if (m >= MAX_TEXTURED_AREA_LIGHT_VERTS)
            {
                return 0;
            }
            out[m++] = { a.data[0] + t * (b.data[0] - a.data[0]),
                         a.data[1] + t * (b.data[1] - a.data[1]) };
        }
    }
    return m;
}

int ClipUvPolyToRect(const QrFloat2D *in, int n, const float uvMin[2], const float uvMax[2],
                     QrFloat2D *out)
{
    QrFloat2D tmp[MAX_TEXTURED_AREA_LIGHT_VERTS];
    int m = ClipUvPolyEdge(in, n, 0, uvMin[0], true, tmp);
    if (m < 3)
    {
        return 0;
    }
    m = ClipUvPolyEdge(tmp, m, 0, uvMax[0], false, out);
    if (m < 3)
    {
        return 0;
    }
    m = ClipUvPolyEdge(out, m, 1, uvMin[1], true, tmp);
    if (m < 3)
    {
        return 0;
    }
    return ClipUvPolyEdge(tmp, m, 1, uvMax[1], false, out);
}

float UvPolyAreaAndCentroid(const QrFloat2D *uv, int n, float outCentroidUv[2])
{
    double area2 = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    for (int i = 0; i < n; i++)
    {
        const QrFloat2D &a = uv[i];
        const QrFloat2D &b = uv[(i + 1) % n];
        const double cross = double(a.data[0]) * double(b.data[1]) -
                             double(b.data[0]) * double(a.data[1]);
        area2 += cross;
        cx += (double(a.data[0]) + double(b.data[0])) * cross;
        cy += (double(a.data[1]) + double(b.data[1])) * cross;
    }

    if (std::fabs(area2) > 1e-12)
    {
        outCentroidUv[0] = float(cx / (3.0 * area2));
        outCentroidUv[1] = float(cy / (3.0 * area2));
    }
    else
    {
        outCentroidUv[0] = uv[0].data[0];
        outCentroidUv[1] = uv[0].data[1];
    }
    return float(0.5 * std::fabs(area2));
}

void InsertClippedGlowPoly(std::vector<EditorClippedGlowPoly> &outPolys,
                           const EditorClippedGlowPoly &poly)
{
    outPolys.push_back(poly);
    std::sort(outPolys.begin(), outPolys.end(),
              [](const EditorClippedGlowPoly &a, const EditorClippedGlowPoly &b) {
                  return a.area > b.area;
              });
    if (outPolys.size() > size_t(kMaxEmissivePolysPerFace))
    {
        outPolys.pop_back();
    }
}

void ComputeUvTileRange(const std::vector<QrFloat2D> &surfUv, int outMin[2], int outMax[2])
{
    float uvMin[2] = { surfUv[0].data[0], surfUv[0].data[1] };
    float uvMax[2] = { surfUv[0].data[0], surfUv[0].data[1] };
    for (size_t i = 1; i < surfUv.size(); i++)
    {
        for (int k = 0; k < 2; k++)
        {
            uvMin[k] = std::min(uvMin[k], surfUv[i].data[k]);
            uvMax[k] = std::max(uvMax[k], surfUv[i].data[k]);
        }
    }
    for (int k = 0; k < 2; k++)
    {
        outMin[k] = int(std::floor(uvMin[k]));
        outMax[k] = int(std::fmax(std::ceil(uvMax[k]) - 1.0, double(outMin[k])));
    }
}

void MeasureFace(const std::vector<QrFloat3D> &corners, float &area, float center[3])
{
    area = 0.0f;
    center[0] = 0.0f;
    center[1] = 0.0f;
    center[2] = 0.0f;
    for (size_t corner = 2; corner < corners.size(); corner++)
    {
        float a[3];
        float b[3];
        float cross[3];
        for (int axis = 0; axis < 3; axis++)
        {
            a[axis] = corners[corner - 1].data[axis] - corners[0].data[axis];
            b[axis] = corners[corner].data[axis] - corners[0].data[axis];
        }
        Cross3(a, b, cross);
        const float triangleArea = 0.5f * Length3(cross);
        if (!(triangleArea > 0.0f))
        {
            continue;
        }
        area += triangleArea;
        for (int axis = 0; axis < 3; axis++)
        {
            center[axis] += triangleArea * (corners[0].data[axis] + corners[corner - 1].data[axis] +
                                            corners[corner].data[axis]) /
                            3.0f;
        }
    }
    if (area > 0.0f)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            center[axis] /= area;
        }
    }
}

void AccumulateUvLeastSquares(const std::vector<QrFloat3D> &corners,
                              const std::vector<QrFloat2D> &texCoords, size_t count,
                              double m[3][3], double rhs[3][3])
{
    double ss = 0.0;
    double st = 0.0;
    double tt = 0.0;
    double ssum = 0.0;
    double tsum = 0.0;
    double sx[3] = {};
    double tx[3] = {};
    double psum[3] = {};
    for (size_t i = 0; i < count; i++)
    {
        const double u = texCoords[i].data[0];
        const double v = texCoords[i].data[1];
        ss += u * u;
        st += u * v;
        tt += v * v;
        ssum += u;
        tsum += v;
        for (int axis = 0; axis < 3; axis++)
        {
            const double p = corners[i].data[axis];
            sx[axis] += u * p;
            tx[axis] += v * p;
            psum[axis] += p;
        }
    }
    m[0][0] = ss;
    m[0][1] = st;
    m[0][2] = ssum;
    m[1][0] = st;
    m[1][1] = tt;
    m[1][2] = tsum;
    m[2][0] = ssum;
    m[2][1] = tsum;
    m[2][2] = double(count);
    for (int axis = 0; axis < 3; axis++)
    {
        rhs[0][axis] = sx[axis];
        rhs[1][axis] = tx[axis];
        rhs[2][axis] = psum[axis];
    }
}

void SwapRows3x3(double m[3][3], double rhs[3][3], int rowA, int rowB)
{
    if (rowA == rowB)
    {
        return;
    }
    for (int k = 0; k < 3; k++)
    {
        std::swap(m[rowA][k], m[rowB][k]);
        std::swap(rhs[rowA][k], rhs[rowB][k]);
    }
}

bool Solve3x3Gaussian(double m[3][3], double rhs[3][3])
{
    for (int col = 0; col < 3; col++)
    {
        int pivot = col;
        for (int row = col + 1; row < 3; row++)
        {
            if (std::fabs(m[row][col]) > std::fabs(m[pivot][col]))
            {
                pivot = row;
            }
        }
        if (std::fabs(m[pivot][col]) < 1e-9)
        {
            return false;
        }
        SwapRows3x3(m, rhs, col, pivot);
        const double inv = 1.0 / m[col][col];
        for (int k = 0; k < 3; k++)
        {
            m[col][k] *= inv;
            rhs[col][k] *= inv;
        }
        for (int row = 0; row < 3; row++)
        {
            if (row == col || std::fabs(m[row][col]) < 1e-10)
            {
                continue;
            }
            const double factor = m[row][col];
            for (int k = 0; k < 3; k++)
            {
                m[row][k] -= factor * m[col][k];
                rhs[row][k] -= factor * rhs[col][k];
            }
        }
    }
    return true;
}

bool FitFaceUvToPosition(const std::vector<QrFloat3D> &corners,
                         const std::vector<QrFloat2D> &texCoords, float A[3], float B[3],
                         float C[3])
{
    const size_t count = std::min(corners.size(), texCoords.size());
    if (count < 3)
    {
        return false;
    }
    double m[3][3] = {};
    double rhs[3][3] = {};
    AccumulateUvLeastSquares(corners, texCoords, count, m, rhs);
    if (!Solve3x3Gaussian(m, rhs))
    {
        return false;
    }
    for (int axis = 0; axis < 3; axis++)
    {
        A[axis] = float(rhs[0][axis]);
        B[axis] = float(rhs[1][axis]);
        C[axis] = float(rhs[2][axis]);
    }
    return true;
}

void EmissiveCone(float focusDegrees, float softDegrees, bool projector, float &angleInner,
                  float &angleOuter)
{
    angleInner = 0.0f;
    angleOuter = 0.0f;
    float outerDegrees = focusDegrees;
    if (!(outerDegrees > 0.0f) || outerDegrees > 90.0f)
    {
        if (!projector)
        {
            return;
        }
        outerDegrees = 45.0f;
    }
    if (softDegrees < 0.0f)
    {
        softDegrees = projector ? 45.0f : outerDegrees * 0.1f;
    }
    softDegrees = std::clamp(softDegrees, 0.05f, outerDegrees);
    angleOuter = outerDegrees * (kDtalPi / 180.0f);
    angleInner = (outerDegrees - softDegrees) * (kDtalPi / 180.0f);
}

bool BuildSquaredAreaLight(const float center[3], float area, QrTexturedAreaLightUploadInfo &info)
{
    info.material = QR_NO_MATERIAL;
    info.numVerts = 4;
    float axis[3] = {
        std::fabs(info.normal.data[2]) > 0.9f ? 1.0f : 0.0f,
        0.0f,
        std::fabs(info.normal.data[2]) > 0.9f ? 0.0f : 1.0f,
    };
    float u[3];
    Cross3(axis, info.normal.data, u);
    if (!Normalize3(u))
    {
        return false;
    }
    float v[3];
    Cross3(info.normal.data, u, v);
    const float side = std::sqrt(area);
    for (int k = 0; k < 3; k++)
    {
        info.A.data[k] = u[k] * side;
        info.B.data[k] = v[k] * side;
        info.C.data[k] = center[k] - 0.5f * side * u[k] - 0.5f * side * v[k];
    }
    info.uvVerts[0] = { 0.0f, 0.0f };
    info.uvVerts[1] = { 1.0f, 0.0f };
    info.uvVerts[2] = { 1.0f, 1.0f };
    info.uvVerts[3] = { 0.0f, 1.0f };
    return true;
}

bool EmitClippedGlowLights(const EditorMaterialEmission &emission,
                           const EditorEmissiveLight &baseLight,
                           const std::vector<QrFloat2D> &surfUv,
                           std::vector<EditorEmissiveLight> &outLights, uint32_t &glowCount)
{
    std::vector<EditorClippedGlowPoly> polys;
    if (!ClipFaceUvToGlowPolygons(surfUv, baseLight.info.A.data, baseLight.info.B.data,
                                   baseLight.info.C.data, emission, polys))
    {
        return false;
    }

    for (const EditorClippedGlowPoly &poly : polys)
    {
        EditorEmissiveLight piece = baseLight;
        piece.info.uniqueID = kEditorFirstEmissiveLightUniqueID + outLights.size();
        piece.info.numVerts = poly.numVerts;
        std::copy_n(poly.uvVerts, MAX_TEXTURED_AREA_LIGHT_VERTS, piece.info.uvVerts);
        piece.info.area = poly.area;
        piece.info.meanEmiss = emission.glowMean;
        piece.info.fit = 1;
        std::copy_n(poly.center, 3, piece.center);

        const float *c = piece.info.color.data;
        piece.power =
            poly.area * emission.glowMean * (0.2125f * c[0] + 0.7154f * c[1] + 0.0721f * c[2]);
        outLights.push_back(piece);
        glowCount++;
    }
    return true;
}

struct SurfaceEmissiveSpec
{
    const EditorMaterialEmission &emission;
    QrMaterial lightMaterial = QR_NO_MATERIAL;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float rawColor[3] = { 1.0f, 1.0f, 1.0f };
    float gain = 0.0f;
    float proxyGain = 1.0f;
    float meanEmiss = 1.0f;
    float angleInner = 0.0f;
    float angleOuter = 0.0f;
    bool projector = false;
    bool lightShafts = false;
    float lightShaftsPower = 1.0f;
    float lightShaftsDistance = 0.0f;
    float lightShaftsFalloff = 1.0f;
    bool masked = false;
    int aliasMaxPolys = 0;
    bool allowModelDtal = false;
    bool isOverlayOnly = false;
    float dtalMinArea = 0.0f;
    float dtalProxyMinArea = 0.0f;
    float materialMinArea = 0.0f;
    int32_t dtalGroups = 0;
    float dtalSpacing = 128.0f;
    bool dtalProxy = false;
    float normalOffset = 0.0f;
    std::string textureName;
};

struct AliasPieceCandidate
{
    EditorEmissiveLight light;
    uint8_t kind = 0;
};

struct AliasInstanceGroup
{
    uint32_t aliasInstanceId = 0;
    int maxPolys = 0;
    std::vector<AliasPieceCandidate> pieces;
};

struct DtalCollectState
{
    const EditorScene &scene;
    std::vector<EditorEmissiveLight> &lights;
    std::vector<EditorGeneratedLightBase> &proxies;
    EditorDtalBuildCounts &counts;
    std::vector<AliasInstanceGroup> &aliasGroups;
    std::unordered_map<uint32_t, size_t> &aliasGroupIndex;
};

bool IsLevelLightModelTexture(const std::string &textureName)
{
    if (textureName.empty())
    {
        return false;
    }
    const std::string lowerTexName = LowerAscii(textureName);
    const size_t slashPos = lowerTexName.find_last_of('/');
    const size_t start = (slashPos == std::string::npos) ? 0 : slashPos + 1;
    const size_t baseColonPos = lowerTexName.find(':', start);
    const std::string base = lowerTexName.substr(
        start, baseColonPos == std::string::npos ? std::string::npos : baseColonPos - start);

    static constexpr const char *kLevelLightModels[] = {
        "flame.mdl",      "flame2.mdl",  "lavaball.mdl", "proj_balllava.mdl",
        "laser.mdl",      "s_light.spr", "b_light.spr",
    };
    for (const char *modelName : kLevelLightModels)
    {
        if (base == modelName)
        {
            return true;
        }
    }
    return false;
}

int AliasModelMaxPolys(const EditorRenderSettings &settings, const std::string &textureName)
{
    int maxPolys = std::clamp(settings.dtalModelMaxPolys, 0, 32);
    if (IsLevelLightModelTexture(textureName))
    {
        maxPolys = std::min(maxPolys, std::clamp(settings.levelLightMaxPolys, 0, 32));
    }
    return maxPolys;
}

bool IsDtalAliasModelFace(const SurfaceEmissiveSpec &spec, const EditorSurfaceFace &face)
{
    return face.aliasInstanceId != 0 && !spec.isOverlayOnly;
}

void CollectFaceEmissiveLights(const SurfaceEmissiveSpec &spec, size_t surfaceIndex,
                               const EditorSurfaceFace &face,
                               std::vector<EditorEmissiveLight> &outLights,
                               EditorDtalBuildCounts &counts)
{
    if (face.positions.size() < 3)
    {
        return;
    }

    float area = 0.0f;
    float center[3] = { 0.0f, 0.0f, 0.0f };
    MeasureFace(face.positions, area, center);
    if (!(area > 0.0f))
    {
        return;
    }

    for (int axis = 0; axis < 3 && spec.normalOffset != 0.0f; axis++)
    {
        center[axis] += face.normal[axis] * spec.normalOffset;
    }

    EditorEmissiveLight light;
    light.surfaceIndex = surfaceIndex;
    light.cluster = face.cluster;
    light.isAliasModel = IsDtalAliasModelFace(spec, face);
    std::copy_n(center, 3, light.center);

    QrTexturedAreaLightUploadInfo &info = light.info;
    info.uniqueID = kEditorFirstEmissiveLightUniqueID + outLights.size();
    info.color = { spec.color[0] * spec.gain, spec.color[1] * spec.gain, spec.color[2] * spec.gain };
    info.normal = { face.normal[0], face.normal[1], face.normal[2] };
    info.area = area;
    info.meanEmiss = spec.meanEmiss;
    info.material = spec.lightMaterial;
    info.angleInner = spec.angleInner;
    info.angleOuter = spec.angleOuter;
    info.projector = spec.projector ? 1.0f : 0.0f;
    info.isStatic = light.isAliasModel ? 0 : 1;
    info.lightShafts = spec.lightShafts ? 1 : 0;
    info.lightShaftsPower = std::max(0.0f, spec.lightShaftsPower);
    info.lightShaftsDistance = std::max(0.0f, spec.lightShaftsDistance);
    info.lightShaftsFalloff = std::max(0.01f, spec.lightShaftsFalloff);

    float fitA[3] = {};
    float fitB[3] = {};
    float fitC[3] = {};
    const bool fitted = FitFaceUvToPosition(face.positions, face.texCoords, fitA, fitB, fitC) &&
                        face.positions.size() <= MAX_TEXTURED_AREA_LIGHT_VERTS;

    if (fitted)
    {
        info.numVerts = int(face.positions.size());
        info.A = { fitA[0], fitA[1], fitA[2] };
        info.B = { fitB[0], fitB[1], fitB[2] };
        info.C = { fitC[0] + face.normal[0] * spec.normalOffset,
                   fitC[1] + face.normal[1] * spec.normalOffset,
                   fitC[2] + face.normal[2] * spec.normalOffset };
        for (int corner = 0; corner < info.numVerts; corner++)
        {
            info.uvVerts[corner] = face.texCoords[size_t(corner)];
        }
        if (spec.masked &&
            EmitClippedGlowLights(spec.emission, light, face.texCoords, outLights,
                                  counts.glowClipped))
        {
            return;
        }
        counts.fitted++;
    }
    else if (BuildSquaredAreaLight(center, area, info))
    {
        counts.squared++;
    }
    else
    {
        return;
    }

    info.fit = 1;
    light.power = area * spec.meanEmiss *
                  (0.2125f * info.color.data[0] + 0.7154f * info.color.data[1] +
                   0.0721f * info.color.data[2]);
    outLights.push_back(light);
}

void CollectAliasFacePieces(const SurfaceEmissiveSpec &spec, size_t surfaceIndex,
                            const EditorSurfaceFace &face,
                            std::vector<AliasPieceCandidate> &outPieces)
{
    std::vector<EditorEmissiveLight> scratch;
    EditorDtalBuildCounts faceCounts;
    CollectFaceEmissiveLights(spec, surfaceIndex, face, scratch, faceCounts);
    const uint8_t kind = faceCounts.glowClipped > 0 ? 1 : (faceCounts.squared > 0 ? 2 : 0);
    for (const EditorEmissiveLight &piece : scratch)
    {
        outPieces.push_back({ piece, kind });
    }
}

void NudgeProxyPositionOutOfSolid(const EditorScene &scene, const float dir[3], float radiusUnits,
                                   float pos[3])
{
    if (scene.worldClusters.numLeafs == 0)
    {
        return;
    }
    for (int step = 0; step < 4; step++)
    {
        const uint32_t cluster = ResolveEditorLightCluster(scene, pos);
        if (cluster != 0 && cluster != QR_CLUSTER_LIGHT_NO_CLUSTER)
        {
            break;
        }
        pos[0] += dir[0] * radiusUnits;
        pos[1] += dir[1] * radiusUnits;
        pos[2] += dir[2] * radiusUnits;
    }
}

void RecordEmissiveLightPiece(EditorEmissiveLight &piece, uint8_t kind,
                              std::vector<EditorEmissiveLight> &outLights,
                              EditorDtalBuildCounts &counts)
{
    piece.info.uniqueID = kEditorFirstEmissiveLightUniqueID + outLights.size();
    outLights.push_back(piece);
    if (kind == 1)
    {
        counts.glowClipped++;
    }
    else if (kind == 2)
    {
        counts.squared++;
    }
    else
    {
        counts.fitted++;
    }
}

struct ProxyLightInput
{
    size_t surfaceIndex = 0;
    size_t faceIndex = 0;
    const float *faceNormal = nullptr;
    const float *proxyCenter = nullptr;
    float proxyArea = 0.0f;
};

void AppendProxyLightForFace(const SurfaceEmissiveSpec &spec, const ProxyLightInput &in,
                             const EditorScene &scene,
                             std::vector<EditorGeneratedLightBase> &outProxies)
{
    const float radiusUnits = std::max(std::sqrt(in.proxyArea / 12.566370614f), 1.0f);
    float pos[3] = {
        in.proxyCenter[0] / in.proxyArea,
        in.proxyCenter[1] / in.proxyArea,
        in.proxyCenter[2] / in.proxyArea,
    };
    float dir[3] = { in.faceNormal[0], in.faceNormal[1], in.faceNormal[2] };
    if (!Normalize3(dir))
    {
        dir[0] = 0.0f;
        dir[1] = 0.0f;
        dir[2] = 1.0f;
    }
    NudgeProxyPositionOutOfSolid(scene, dir, radiusUnits, pos);

    EditorGeneratedLightBase proxy;
    proxy.name = spec.textureName;
    proxy.uniqueID =
        (uint64_t(1) << 60) | (uint64_t(in.surfaceIndex + 1) << 32) | uint64_t(in.faceIndex);
    std::copy_n(pos, 3, proxy.position);
    std::copy_n(spec.rawColor, 3, proxy.baseColor);
    proxy.baseGain = spec.proxyGain * spec.meanEmiss;
    proxy.defaultRadiusMeters = radiusUnits / kEditorUnitsPerMeter;
    proxy.modelSizeMeters = 0.0f;
    proxy.lightShafts = spec.lightShafts;
    proxy.lightShaftsPower = spec.lightShaftsPower;
    proxy.lightShaftsDistance = spec.lightShaftsDistance;
    proxy.lightShaftsFalloff = spec.lightShaftsFalloff;
    proxy.isAliasModel = false;

    if (spec.angleOuter > 0.0f && spec.angleOuter <= 1.5707963f)
    {
        constexpr float kRadToDeg = 180.0f / kDtalPi;
        proxy.spot = true;
        std::copy_n(dir, 3, proxy.direction);
        proxy.angleInner = spec.angleInner * kRadToDeg;
        proxy.angleOuter = spec.angleOuter * kRadToDeg;
    }
    outProxies.push_back(std::move(proxy));
}

struct BrushDtalCluster
{
    EditorEmissiveLight light;
    uint8_t kind = 0;
    size_t firstFaceIndex = 0;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    float uvMin[2] = {};
    float uvMax[2] = {};
    std::vector<QrFloat3D> worldCorners;
    bool active = true;
};

void InitBrushDtalCluster(const EditorEmissiveLight &piece, uint8_t kind, size_t faceIndex,
                          BrushDtalCluster &out)
{
    out = {};
    out.light = piece;
    out.kind = kind;
    out.firstFaceIndex = faceIndex;
    out.active = true;

    const int n = std::clamp(piece.info.numVerts, 0, MAX_TEXTURED_AREA_LIGHT_VERTS);
    out.worldCorners.reserve(size_t(n));
    for (int k = 0; k < n; k++)
    {
        const float u = piece.info.uvVerts[size_t(k)].data[0];
        const float v = piece.info.uvVerts[size_t(k)].data[1];
        QrFloat3D p{};
        for (int axis = 0; axis < 3; axis++)
        {
            p.data[axis] = piece.info.A.data[axis] * u + piece.info.B.data[axis] * v +
                           piece.info.C.data[axis];
            out.boundsMin[axis] =
                k == 0 ? p.data[axis] : std::min(out.boundsMin[axis], p.data[axis]);
            out.boundsMax[axis] =
                k == 0 ? p.data[axis] : std::max(out.boundsMax[axis], p.data[axis]);
        }
        out.uvMin[0] = k == 0 ? u : std::min(out.uvMin[0], u);
        out.uvMax[0] = k == 0 ? u : std::max(out.uvMax[0], u);
        out.uvMin[1] = k == 0 ? v : std::min(out.uvMin[1], v);
        out.uvMax[1] = k == 0 ? v : std::max(out.uvMax[1], v);
        out.worldCorners.push_back(p);
    }
}

bool CanMergeBrushDtalClusters(const BrushDtalCluster &a, const BrushDtalCluster &b, float maxGap)
{
    if (!a.active || !b.active || Dot3(a.light.info.normal.data, b.light.info.normal.data) < 0.5f)
    {
        return false;
    }
    for (int axis = 0; axis < 3; axis++)
    {
        if (a.boundsMin[axis] > b.boundsMax[axis] + maxGap ||
            b.boundsMin[axis] > a.boundsMax[axis] + maxGap)
        {
            return false;
        }
    }
    return true;
}

void BuildTangentBasisFromNormal(const float normal[3], const float hintA[3], float outU[3],
                                 float outV[3])
{
    const float proj = Dot3(hintA, normal);
    for (int k = 0; k < 3; k++)
    {
        outU[k] = hintA[k] - normal[k] * proj;
    }
    if (!Normalize3(outU))
    {
        const float fallback[3] = {
            std::fabs(normal[2]) > 0.9f ? 1.0f : 0.0f,
            0.0f,
            std::fabs(normal[2]) > 0.9f ? 0.0f : 1.0f,
        };
        Cross3(fallback, normal, outU);
        if (!Normalize3(outU))
        {
            outU[0] = 1.0f;
            outU[1] = 0.0f;
            outU[2] = 0.0f;
        }
    }
    Cross3(normal, outU, outV);
    if (!Normalize3(outV))
    {
        outV[0] = 0.0f;
        outV[1] = 1.0f;
        outV[2] = 0.0f;
    }
}

void ComputeClusterTangentExtents(const std::vector<QrFloat3D> &corners, const float center[3],
                                  const float uAxis[3], const float vAxis[3], float outBounds[4])
{
    for (size_t i = 0; i < corners.size(); i++)
    {
        const float rel[3] = {
            corners[i].data[0] - center[0],
            corners[i].data[1] - center[1],
            corners[i].data[2] - center[2],
        };
        const float du = Dot3(rel, uAxis);
        const float dv = Dot3(rel, vAxis);
        if (i == 0)
        {
            outBounds[0] = du;
            outBounds[1] = du;
            outBounds[2] = dv;
            outBounds[3] = dv;
            continue;
        }
        outBounds[0] = std::min(outBounds[0], du);
        outBounds[1] = std::max(outBounds[1], du);
        outBounds[2] = std::min(outBounds[2], dv);
        outBounds[3] = std::max(outBounds[3], dv);
    }
}

void SelectClusterUvRect(const EditorMaterialEmission &emission, const BrushDtalCluster &dst,
                         float outUv[4])
{
    if (emission.hasGlow)
    {
        outUv[0] = emission.glowUvMin[0];
        outUv[1] = emission.glowUvMax[0];
        outUv[2] = emission.glowUvMin[1];
        outUv[3] = emission.glowUvMax[1];
        return;
    }
    if ((dst.uvMax[0] - dst.uvMin[0]) > 1.0e-4f && (dst.uvMax[1] - dst.uvMin[1]) > 1.0e-4f)
    {
        outUv[0] = dst.uvMin[0];
        outUv[1] = dst.uvMax[0];
        outUv[2] = dst.uvMin[1];
        outUv[3] = dst.uvMax[1];
        return;
    }
    outUv[0] = 0.0f;
    outUv[1] = 1.0f;
    outUv[2] = 0.0f;
    outUv[3] = 1.0f;
}

void RebuildMergedClusterQuad(const EditorMaterialEmission &emission, BrushDtalCluster &dst)
{
    if (dst.worldCorners.empty())
    {
        return;
    }
    float uAxis[3] = {};
    float vAxis[3] = {};
    BuildTangentBasisFromNormal(dst.light.info.normal.data, dst.light.info.A.data, uAxis, vAxis);

    float bounds[4] = {};
    ComputeClusterTangentExtents(dst.worldCorners, dst.light.center, uAxis, vAxis, bounds);

    float uv[4] = {};
    SelectClusterUvRect(emission, dst, uv);

    const float spanU = std::max(bounds[1] - bounds[0], 1.0e-2f);
    const float spanV = std::max(bounds[3] - bounds[2], 1.0e-2f);
    const float duUv = std::max(uv[1] - uv[0], 1.0e-4f);
    const float dvUv = std::max(uv[3] - uv[2], 1.0e-4f);

    for (int k = 0; k < 3; k++)
    {
        dst.light.info.A.data[k] = uAxis[k] * (spanU / duUv);
        dst.light.info.B.data[k] = vAxis[k] * (spanV / dvUv);
        const float p0 = dst.light.center[k] + uAxis[k] * bounds[0] + vAxis[k] * bounds[2];
        dst.light.info.C.data[k] =
            p0 - dst.light.info.A.data[k] * uv[0] - dst.light.info.B.data[k] * uv[2];
    }

    dst.light.info.numVerts = 4;
    dst.light.info.uvVerts[0] = { uv[0], uv[2] };
    dst.light.info.uvVerts[1] = { uv[1], uv[2] };
    dst.light.info.uvVerts[2] = { uv[1], uv[3] };
    dst.light.info.uvVerts[3] = { uv[0], uv[3] };
    dst.light.info.fit = 1;
}

void MergeTwoBrushDtalClusters(const EditorMaterialEmission &emission, BrushDtalCluster &dst,
                               const BrushDtalCluster &src)
{
    const float areaA = dst.light.info.area;
    const float areaB = src.light.info.area;
    const float areaSum = std::max(areaA + areaB, 1.0e-4f);

    float combinedNormal[3] = {};
    for (int axis = 0; axis < 3; axis++)
    {
        dst.light.center[axis] =
            (dst.light.center[axis] * areaA + src.light.center[axis] * areaB) / areaSum;
        combinedNormal[axis] =
            dst.light.info.normal.data[axis] * areaA + src.light.info.normal.data[axis] * areaB;
        dst.boundsMin[axis] = std::min(dst.boundsMin[axis], src.boundsMin[axis]);
        dst.boundsMax[axis] = std::max(dst.boundsMax[axis], src.boundsMax[axis]);
    }
    if (Normalize3(combinedNormal))
    {
        std::copy_n(combinedNormal, 3, dst.light.info.normal.data);
    }

    dst.uvMin[0] = std::min(dst.uvMin[0], src.uvMin[0]);
    dst.uvMin[1] = std::min(dst.uvMin[1], src.uvMin[1]);
    dst.uvMax[0] = std::max(dst.uvMax[0], src.uvMax[0]);
    dst.uvMax[1] = std::max(dst.uvMax[1], src.uvMax[1]);
    dst.worldCorners.insert(dst.worldCorners.end(), src.worldCorners.begin(),
                            src.worldCorners.end());

    dst.light.info.area = areaSum;
    dst.light.power += src.light.power;
    if (dst.light.cluster == 0u || dst.light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER)
    {
        dst.light.cluster = src.light.cluster;
    }
    RebuildMergedClusterQuad(emission, dst);
}

size_t FindNearestMergePartner(const std::vector<BrushDtalCluster> &clusters, size_t idx,
                               float maxGap)
{
    float minDistSq = std::numeric_limits<float>::infinity();
    size_t partner = size_t(-1);
    const BrushDtalCluster &c = clusters[idx];
    for (size_t j = 0; j < clusters.size(); j++)
    {
        if (idx == j || !CanMergeBrushDtalClusters(c, clusters[j], maxGap))
        {
            continue;
        }
        const float dx = clusters[j].light.center[0] - c.light.center[0];
        const float dy = clusters[j].light.center[1] - c.light.center[1];
        const float dz = clusters[j].light.center[2] - c.light.center[2];
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < minDistSq)
        {
            minDistSq = distSq;
            partner = j;
        }
    }
    return partner;
}

bool FindBestDtalMergePair(const std::vector<BrushDtalCluster> &clusters, float targetArea,
                           bool groupAll, float maxGap, size_t &outA, size_t &outB)
{
    outA = size_t(-1);
    outB = size_t(-1);
    float bestArea = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < clusters.size(); i++)
    {
        const BrushDtalCluster &c = clusters[i];
        if (!c.active || (!groupAll && !(c.light.info.area < targetArea)) ||
            !(c.light.info.area < bestArea))
        {
            continue;
        }
        const size_t partner = FindNearestMergePartner(clusters, i, maxGap);
        if (partner != size_t(-1))
        {
            bestArea = c.light.info.area;
            outA = i;
            outB = partner;
        }
    }
    return outA != size_t(-1) && outB != size_t(-1);
}

void EmitSurfaceBrushDtalClusters(const SurfaceEmissiveSpec &spec, size_t surfaceIndex,
                                  std::vector<BrushDtalCluster> &clusters, DtalCollectState &state)
{
    const float targetArea = std::max(spec.dtalMinArea, spec.materialMinArea);
    const bool groupAll = spec.dtalGroups > 0;
    const float maxGap =
        (spec.dtalGroups >= 2)
            ? std::max(16.0f, spec.dtalSpacing)
            : ((spec.dtalGroups == 1) ? std::clamp(spec.dtalSpacing * 0.25f, 8.0f, 48.0f) : 16.0f);

    if (targetArea > 0.0f || groupAll)
    {
        size_t idxA = 0;
        size_t idxB = 0;
        while (FindBestDtalMergePair(clusters, targetArea, groupAll, maxGap, idxA, idxB))
        {
            MergeTwoBrushDtalClusters(spec.emission, clusters[idxA], clusters[idxB]);
            clusters[idxB].active = false;
        }
    }

    for (BrushDtalCluster &cluster : clusters)
    {
        if (!cluster.active)
        {
            continue;
        }
        const float area = cluster.light.info.area;
        if (spec.dtalMinArea > 0.0f && area < spec.dtalMinArea)
        {
            const float proxyFloor = std::min(spec.dtalMinArea, spec.dtalProxyMinArea);
            if (spec.dtalProxy && area > 0.0f && area >= proxyFloor)
            {
                const float weightedCenter[3] = {
                    cluster.light.center[0] * area,
                    cluster.light.center[1] * area,
                    cluster.light.center[2] * area,
                };
                const ProxyLightInput proxyIn{
                    surfaceIndex,
                    cluster.firstFaceIndex,
                    cluster.light.info.normal.data,
                    weightedCenter,
                    area,
                };
                AppendProxyLightForFace(spec, proxyIn, state.scene, state.proxies);
            }
            continue;
        }
        RecordEmissiveLightPiece(cluster.light, cluster.kind, state.lights, state.counts);
    }
}

void AppendSurfaceEmissiveFaces(const SurfaceEmissiveSpec &spec, size_t surfaceIndex,
                                const EditorSurface &surface, DtalCollectState &state)
{
    std::vector<BrushDtalCluster> brushClusters;
    std::vector<EditorEmissiveLight> scratch;

    for (size_t faceIndex = 0; faceIndex < surface.faces.size(); faceIndex++)
    {
        const EditorSurfaceFace &face = surface.faces[faceIndex];
        if (!IsDtalAliasModelFace(spec, face))
        {
            scratch.clear();
            EditorDtalBuildCounts faceCounts;
            CollectFaceEmissiveLights(spec, surfaceIndex, face, scratch, faceCounts);
            const uint8_t kind = faceCounts.glowClipped > 0 ? 1 : (faceCounts.squared > 0 ? 2 : 0);
            for (const EditorEmissiveLight &piece : scratch)
            {
                BrushDtalCluster cluster;
                InitBrushDtalCluster(piece, kind, faceIndex, cluster);
                brushClusters.push_back(std::move(cluster));
            }
            continue;
        }
        if (!spec.allowModelDtal || !spec.masked || !(spec.emission.meanBase > 0.0f))
        {
            continue;
        }
        auto [it, inserted] = state.aliasGroupIndex.emplace(face.aliasInstanceId, state.aliasGroups.size());
        if (inserted)
        {
            state.aliasGroups.push_back(
                AliasInstanceGroup{ face.aliasInstanceId, spec.aliasMaxPolys, {} });
        }
        CollectAliasFacePieces(spec, surfaceIndex, face, state.aliasGroups[it->second].pieces);
    }

    if (!brushClusters.empty())
    {
        EmitSurfaceBrushDtalClusters(spec, surfaceIndex, brushClusters, state);
    }
}

void AppendCappedAliasInstanceLights(std::vector<AliasInstanceGroup> &aliasGroups,
                                     int32_t modelBudget,
                                     std::vector<EditorEmissiveLight> &outLights,
                                     EditorDtalBuildCounts &counts,
                                     std::vector<uint32_t> &outQualifiedAliasIds)
{
    int32_t remainingBudget = std::max<int32_t>(0, modelBudget);
    for (AliasInstanceGroup &group : aliasGroups)
    {
        if (remainingBudget <= 0 || group.maxPolys <= 0 || group.pieces.empty())
        {
            continue;
        }
        std::stable_sort(group.pieces.begin(), group.pieces.end(),
                         [](const AliasPieceCandidate &a, const AliasPieceCandidate &b) {
                             return a.light.info.area > b.light.info.area;
                         });

        const int32_t takeCount =
            std::min<int32_t>(int32_t(group.pieces.size()), std::min(group.maxPolys, remainingBudget));
        if (takeCount > 0)
        {
            outQualifiedAliasIds.push_back(group.aliasInstanceId);
        }
        for (int32_t i = 0; i < takeCount; i++)
        {
            RecordEmissiveLightPiece(group.pieces[size_t(i)].light, group.pieces[size_t(i)].kind,
                                     outLights, counts);
            remainingBudget--;
        }
    }
}

void CollectAliasBases(const EditorScene &scene, const EditorMaterialTable &materials,
                       const EditorRenderSettings &settings,
                       const std::vector<uint32_t> &qualifiedAliasIds,
                       std::vector<EditorGeneratedLightBase> &outBases)
{
    if (!(settings.truelight < 2.0f))
    {
        return;
    }

    for (const EditorAliasInstance &inst : scene.aliasInstances)
    {
        if (inst.uniqueID == 0 ||
            std::find(qualifiedAliasIds.begin(), qualifiedAliasIds.end(), inst.aliasInstanceId) !=
                qualifiedAliasIds.end())
        {
            continue;
        }
        int32_t matIdx = materials.IndexOf(inst.textureName);
        if (matIdx < 0)
        {
            matIdx = materials.IndexOf(inst.modelName);
        }
        if (matIdx < 0)
        {
            continue;
        }
        const EditorMaterialParams &params = materials.At(size_t(matIdx));
        if (!params.enabled || !params.luma || !params.hasLightColor ||
            !(params.lightBrightness > 0.0f))
        {
            continue;
        }

        EditorGeneratedLightBase base;
        base.name = inst.textureName;
        base.uniqueID = inst.uniqueID;
        std::copy_n(inst.origin, 3, base.position);
        std::copy_n(params.lightColor, 3, base.baseColor);
        base.baseGain = params.lightBrightness * settings.emissionLightIntensity;
        base.defaultRadiusMeters = 0.1f;
        base.modelSizeMeters = std::max(inst.modelSizeMeters, 0.001f);
        base.lightShafts = params.lightShafts;
        base.lightShaftsPower = params.lightShaftsPower;
        base.lightShaftsDistance = params.lightShaftsDistance;
        base.lightShaftsFalloff = params.lightShaftsFalloff;
        base.isAliasModel = true;
        outBases.push_back(std::move(base));
    }
}

struct SurfaceLightModeParams
{
    bool masked = false;
    float brightness = 0.0f;
    float minArea = 0.0f;
    bool proxy = false;
    float normalOffset = 0.0f;
    float color[3] = { 1.0f, 1.0f, 1.0f };
};

void ResolveAutoEmissiveFallback(const EditorScene &scene, const EditorSurface &surface,
                                 EditorMaterialEmission &emission)
{
    const bool zeroColor =
        (emission.color[0] + emission.color[1] + emission.color[2]) < 1.0e-3f;
    if (emission.valid && emission.meanBase > 1.0e-6f && !zeroColor)
    {
        return;
    }

    const EditorTexture *tex = FindEditorSurfaceTexture(scene, surface);
    if (tex != nullptr && tex->hasSurfaceLightColor)
    {
        std::copy_n(tex->surfaceLightColor, 3, emission.color);
    }
    else if (zeroColor || !emission.valid || emission.meanBase <= 1.0e-6f)
    {
        emission.color[0] = 1.0f;
        emission.color[1] = 1.0f;
        emission.color[2] = 1.0f;
    }

    emission.valid = true;
    emission.mean = std::max(emission.mean, 0.25f);
    emission.meanBase = std::max(emission.meanBase, 0.25f);
}

SurfaceLightModeParams ResolveSurfaceLightModeParams(const EditorScene &scene,
                                                     const EditorSurface &surface,
                                                     const EditorMaterialParams &params,
                                                     const EditorRenderSettings &settings,
                                                     bool explicitDtal,
                                                     EditorMaterialEmission &emission)
{
    SurfaceLightModeParams out;
    out.masked = emission.valid && emission.meanBase > 1.0e-6f;
    if (!explicitDtal)
    {
        ResolveAutoEmissiveFallback(scene, surface, emission);
    }
    out.brightness = explicitDtal ? params.lightBrightness : (params.emissiveFactor * 0.35f);
    out.minArea = explicitDtal ? settings.dtalMinArea : std::max(settings.dtalMinArea, 64.0f);
    out.proxy = explicitDtal ? settings.dtalProxy : false;
    out.normalOffset = ComputeEditorSurfaceEmissiveNormalOffset(scene, surface, params);
    const float *baseColor =
        explicitDtal ? ((params.hasLightColor || !out.masked) ? params.lightColor : emission.color)
                     : emission.color;
    std::copy_n(baseColor, 3, out.color);
    return out;
}

struct UvAffine2D
{
    float m00 = 1.0f;
    float m01 = 0.0f;
    float tx = 0.0f;
    float m10 = 0.0f;
    float m11 = 1.0f;
    float ty = 0.0f;
};

void ComposeUvAffine(UvAffine2D &a, float n00, float n01, float nx, float n10, float n11, float ny)
{
    const UvAffine2D next{
        n00 * a.m00 + n01 * a.m10,
        n00 * a.m01 + n01 * a.m11,
        n00 * a.tx + n01 * a.ty + nx,
        n10 * a.m00 + n11 * a.m10,
        n10 * a.m01 + n11 * a.m11,
        n10 * a.tx + n11 * a.ty + ny,
    };
    a = next;
}

void AccumulateTcModAffine(const EditorTcMod &mod, float timeSeconds, UvAffine2D &xform)
{
    switch (mod.type)
    {
    case EditorTcModType::Scale:
        ComposeUvAffine(xform, mod.params[0], 0.0f, 0.0f, 0.0f, mod.params[1], 0.0f);
        break;
    case EditorTcModType::Scroll:
        ComposeUvAffine(xform, 1.0f, 0.0f, mod.params[0] * timeSeconds, 0.0f, 1.0f,
                        mod.params[1] * timeSeconds);
        break;
    case EditorTcModType::Rotate:
    {
        const float rad = -mod.params[0] * timeSeconds * (kDtalPi / 180.0f);
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        ComposeUvAffine(xform, c, -s, 0.5f - 0.5f * c + 0.5f * s, s, c,
                        0.5f - 0.5f * s - 0.5f * c);
        break;
    }
    case EditorTcModType::Stretch:
    {
        const float w = EvaluateEditorWave(mod.wave, mod.params[0], mod.params[1], mod.params[2],
                                           mod.params[3], timeSeconds);
        const float inv = 1.0f / std::max(0.001f, w);
        const float shift = 0.5f * (1.0f - inv);
        ComposeUvAffine(xform, inv, 0.0f, shift, 0.0f, inv, shift);
        break;
    }
    case EditorTcModType::Turb:
    {
        const float amp = mod.params[1] * 0.5f;
        const float phase = (mod.params[2] + timeSeconds * mod.params[3]) * 2.0f * kDtalPi;
        ComposeUvAffine(xform, 1.0f, 0.0f, std::sin(phase) * amp, 0.0f, 1.0f,
                        std::cos(phase) * amp);
        break;
    }
    }
}

const std::vector<EditorTcMod> *FindSurfaceAnimatedTcMods(const EditorScene &scene,
                                                          const EditorMaterialTable &materials,
                                                          size_t surfaceIndex)
{
    if (surfaceIndex >= scene.surfaces.size())
    {
        return nullptr;
    }
    const EditorSurface &surface = scene.surfaces[surfaceIndex];
    const EditorTexture *tex = FindEditorSurfaceTexture(scene, surface);
    if (tex == nullptr)
    {
        return nullptr;
    }
    const uint32_t disabledMask = materials.For(surface).disabledStagesMask;
    for (size_t i = 0; i < tex->shaderStages.size() && i < 32u; ++i)
    {
        if (i == tex->baseStageIndex || ((disabledMask >> i) & 1u) != 0u)
        {
            continue;
        }
        const EditorOverlayStage &stage = tex->shaderStages[i];
        if (!stage.isLightmap && !stage.tcMods.empty())
        {
            return &stage.tcMods;
        }
    }
    return nullptr;
}

void ApplySurfaceAnimatedTcModsToAreaLight(const EditorScene &scene,
                                           const EditorMaterialTable &materials,
                                           size_t surfaceIndex, float timeSeconds,
                                           QrTexturedAreaLightUploadInfo &info)
{
    if (info.fit == 0 || info.numVerts < 3)
    {
        return;
    }
    const std::vector<EditorTcMod> *tcMods =
        FindSurfaceAnimatedTcMods(scene, materials, surfaceIndex);
    if (tcMods == nullptr)
    {
        return;
    }

    UvAffine2D m;
    for (const EditorTcMod &mod : *tcMods)
    {
        AccumulateTcModAffine(mod, timeSeconds, m);
    }
    const float det = m.m00 * m.m11 - m.m01 * m.m10;
    if (std::abs(det) < 1.0e-6f)
    {
        return;
    }
    const float invDet = 1.0f / det;
    const QrFloat3D oldA = info.A;
    const QrFloat3D oldB = info.B;
    for (int axis = 0; axis < 3; axis++)
    {
        const float newA = (m.m11 * oldA.data[axis] - m.m10 * oldB.data[axis]) * invDet;
        const float newB = (-m.m01 * oldA.data[axis] + m.m00 * oldB.data[axis]) * invDet;
        info.A.data[axis] = newA;
        info.B.data[axis] = newB;
        info.C.data[axis] -= m.tx * newA + m.ty * newB;
    }
    const int n = std::min(info.numVerts, int(MAX_TEXTURED_AREA_LIGHT_VERTS));
    for (int v = 0; v < n; v++)
    {
        const float u = info.uvVerts[v].data[0];
        const float w = info.uvVerts[v].data[1];
        info.uvVerts[v].data[0] = m.m00 * u + m.m01 * w + m.tx;
        info.uvVerts[v].data[1] = m.m10 * u + m.m11 * w + m.ty;
    }
}

struct ModelTriRank
{
    uint32_t triIndex = 0;
    float area = 0.0f;
    float center[3] = {};
    float normal[3] = {};
};

std::vector<ModelTriRank> CollectRankedModelTriangles(const std::vector<QrVertex> &vertices,
                                                      const std::vector<uint32_t> &indices)
{
    std::vector<ModelTriRank> ranked;
    ranked.reserve(indices.size() / 3u);
    for (size_t i = 0; i + 2 < indices.size(); i += 3u)
    {
        const uint32_t i0 = indices[i + 0];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            continue;
        }
        const float *p0 = vertices[i0].position;
        const float *p1 = vertices[i1].position;
        const float *p2 = vertices[i2].position;
        const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        ModelTriRank rank;
        rank.triIndex = uint32_t(i);
        Cross3(e1, e2, rank.normal);
        rank.area = 0.5f * Length3(rank.normal);
        if (!(rank.area > 0.5f) || !Normalize3(rank.normal))
        {
            continue;
        }
        const float vNorm[3] = {
            vertices[i0].normal[0] + vertices[i1].normal[0] + vertices[i2].normal[0],
            vertices[i0].normal[1] + vertices[i1].normal[1] + vertices[i2].normal[1],
            vertices[i0].normal[2] + vertices[i1].normal[2] + vertices[i2].normal[2],
        };
        if (Dot3(vNorm, vNorm) > 1.0e-4f && Dot3(rank.normal, vNorm) < 0.0f)
        {
            rank.normal[0] = -rank.normal[0];
            rank.normal[1] = -rank.normal[1];
            rank.normal[2] = -rank.normal[2];
        }
        for (int axis = 0; axis < 3; axis++)
        {
            rank.center[axis] = (p0[axis] + p1[axis] + p2[axis]) / 3.0f;
        }
        ranked.push_back(rank);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const ModelTriRank &a, const ModelTriRank &b) { return a.area > b.area; });
    return ranked;
}

void ApplyGeneratedOverrideSpotAndOffset(const EditorGeneratedLightBase &base,
                                         const EditorEmitterLightOverride *ov, EditorLight &light)
{
    light.spot = base.spot;
    std::copy_n(base.direction, 3, light.direction);
    light.angleInner = base.angleInner;
    light.angleOuter = base.angleOuter;
    light.lightShafts = base.lightShafts;
    light.lightShaftsPower = base.lightShaftsPower;
    light.lightShaftsDistance = base.lightShaftsDistance;
    light.lightShaftsFalloff = base.lightShaftsFalloff;
    light.groupEdit = true;
    light.hasStyle = false;

    if (ov != nullptr)
    {
        if (ov->hasSpot)
        {
            light.spot = ov->spot;
        }
        if (ov->hasDir && !ov->groupEdit)
        {
            std::copy_n(ov->dir, 3, light.direction);
        }
        if (ov->hasAngles)
        {
            light.angleInner = ov->angleInner;
            light.angleOuter = ov->angleOuter;
        }
        if (ov->hasOffset)
        {
            light.hasOffset = true;
            std::copy_n(ov->offset, 3, light.offset);
        }
        light.hasStyle = ov->hasStyle;
        if (ov->hasStyle)
        {
            light.style = ov->style;
        }
        light.forceRasterize = ov->forceRasterize;
        light.groupEdit = ov->groupEdit;
        light.enabled = ov->enabled;
    }

    ClampEditorLightCone(light);
}

void ApplyGeneratedOverrideRadiometry(const EditorGeneratedLightBase &base,
                                      const EditorEmitterLightOverride *ov,
                                      const EditorRenderSettings &settings, EditorLight &light)
{
    const float ovRadius = (ov != nullptr && ov->hasRadius) ? ov->radius : base.defaultRadiusMeters;
    const float radiusScale = base.modelSizeMeters > 0.0f ? base.modelSizeMeters : 1.0f;
    light.radius = std::max(ovRadius * radiusScale * kEditorUnitsPerMeter, 0.5f);

    const float *rgb = base.baseColor;
    float baseGain = base.baseGain;
    float ovIntensity = 1.0f;

    if (ov != nullptr)
    {
        if (ov->hasColor)
        {
            rgb = ov->color;
            baseGain = 1.0f;
            light.hasColor = true;
        }
        if (ov->hasIntensity)
        {
            ovIntensity = ov->intensity;
        }
    }

    const float scaledIntensity = ovIntensity * baseGain * std::max(0.0f, settings.globalLightMult) *
                                  std::max(0.0f, settings.brightness) * 10.0f;
    SetEditorLightTint(light, rgb);
    SetEditorLightIntensity(light, scaledIntensity);
}

} // namespace

bool ClipFaceUvToGlowPolygons(const std::vector<QrFloat2D> &surfUv, const float A[3],
                               const float B[3], const float C[3],
                               const EditorMaterialEmission &emission,
                               std::vector<EditorClippedGlowPoly> &outPolys)
{
    outPolys.clear();
    if (!emission.hasGlow || surfUv.size() < 3 || surfUv.size() > MAX_TEXTURED_AREA_LIGHT_VERTS)
    {
        return false;
    }

    int tileMin[2] = {};
    int tileMax[2] = {};
    ComputeUvTileRange(surfUv, tileMin, tileMax);
    const double tileSpanX = double(tileMax[0]) - double(tileMin[0]) + 1.0;
    const double tileSpanY = double(tileMax[1]) - double(tileMin[1]) + 1.0;
    if (tileSpanX <= 0.0 || tileSpanY <= 0.0 ||
        tileSpanX * tileSpanY > double(kMaxEmissivePolysPerFace))
    {
        return false;
    }

    float cross[3] = {};
    Cross3(A, B, cross);
    const float jacobian = Length3(cross);

    for (int ty = tileMin[1]; ty <= tileMax[1]; ty++)
    {
        for (int tx = tileMin[0]; tx <= tileMax[0]; tx++)
        {
            const float uvMinTile[2] = { float(tx) + emission.glowUvMin[0],
                                         float(ty) + emission.glowUvMin[1] };
            const float uvMaxTile[2] = { float(tx) + emission.glowUvMax[0],
                                         float(ty) + emission.glowUvMax[1] };

            EditorClippedGlowPoly poly;
            poly.numVerts = ClipUvPolyToRect(surfUv.data(), int(surfUv.size()), uvMinTile,
                                             uvMaxTile, poly.uvVerts);
            if (poly.numVerts < 3)
            {
                continue;
            }
            float uvCentroid[2] = {};
            const float uvArea = UvPolyAreaAndCentroid(poly.uvVerts, poly.numVerts, uvCentroid);
            if (uvArea <= 1e-9f)
            {
                continue;
            }
            poly.area = uvArea * jacobian;
            for (int k = 0; k < 3; k++)
            {
                poly.center[k] = A[k] * uvCentroid[0] + B[k] * uvCentroid[1] + C[k];
            }
            InsertClippedGlowPoly(outPolys, poly);
        }
    }
    return true;
}

namespace
{

struct SurfaceBuildContext
{
    const EditorScene &scene;
    const EditorMaterialTable &materials;
    const EditorRenderSettings &settings;
    const EditorDtalMaterialLookup &lookup;
    float intensityScale = 0.0f;
    std::vector<AliasInstanceGroup> &aliasGroups;
    std::unordered_map<uint32_t, size_t> &aliasGroupIndex;
    EditorDtalBuildResult &out;
    std::vector<EditorGeneratedLightBase> &proxyBases;
};

void CollectSurfaceForSceneEmissiveLights(const SurfaceBuildContext &ctx, size_t surfaceIndex)
{
    const EditorSurface &surface = ctx.scene.surfaces[surfaceIndex];
    if (surface.faces.empty() || surface.kind == EditorSurfaceKind::Sky)
    {
        return;
    }

    const EditorMaterialParams &params = ctx.materials.For(surface);
    const bool explicitDtal = EditorSurfaceUsesDtalLighting(ctx.settings.truelight, params) &&
                              params.lightBrightness > 0.0f;
    const bool autoEmissive = !explicitDtal && ctx.settings.emissiveNee &&
                              EditorSurfaceHasAutoEmissiveSource(ctx.scene, surface, params);
    if (!explicitDtal && !autoEmissive)
    {
        return;
    }

    const QrMaterial material =
        ctx.lookup.surfaceMaterial ? ctx.lookup.surfaceMaterial(surface, params) : QR_NO_MATERIAL;
    EditorMaterialEmission emission =
        ctx.lookup.materialEmission ? ctx.lookup.materialEmission(material) : EditorMaterialEmission{};
    const SurfaceLightModeParams mode =
        ResolveSurfaceLightModeParams(ctx.scene, surface, params, ctx.settings, explicitDtal,
                                      emission);

    float angleInner = 0.0f;
    float angleOuter = 0.0f;
    EmissiveCone(params.lightFocus, params.lightFocusSoft, params.lightProjector, angleInner,
                 angleOuter);

    SurfaceEmissiveSpec spec{
        emission,
        mode.masked ? material : QR_NO_MATERIAL,
        { mode.color[0], mode.color[1], mode.color[2] },
        { mode.color[0], mode.color[1], mode.color[2] },
        mode.brightness * ctx.intensityScale,
        mode.brightness * ctx.settings.emissionLightIntensity,
        mode.masked ? emission.mean : 1.0f,
        angleInner,
        angleOuter,
        params.lightProjector,
        params.lightShafts,
        params.lightShaftsPower,
        params.lightShaftsDistance,
        params.lightShaftsFalloff,
        mode.masked,
        AliasModelMaxPolys(ctx.settings, surface.textureName),
        IsLevelLightModelTexture(surface.textureName) ? ctx.settings.levelLights
                                                      : ctx.settings.modelLights,
        surface.isOverlayOnly,
        mode.minArea,
        ctx.settings.dtalProxyMinArea,
        params.lightMinArea,
        ctx.settings.dtalGroups,
        ctx.settings.dtalSpacing,
        mode.proxy,
        mode.normalOffset,
        surface.textureName,
    };
    DtalCollectState collectState{
        ctx.scene, ctx.out.emissiveLights, ctx.proxyBases, ctx.out.counts, ctx.aliasGroups,
        ctx.aliasGroupIndex,
    };
    AppendSurfaceEmissiveFaces(spec, surfaceIndex, surface, collectState);
}

struct DynamicTriEmitParams
{
    const EditorDynamicMeshDtalRequest &request;
    const float *colorPtr = nullptr;
    float gain = 0.0f;
    bool masked = false;
    float innerRad = 0.0f;
    float outerRad = 0.0f;
};

bool AppendDynamicMeshTriangleLight(const DynamicTriEmitParams &emit, const ModelTriRank &tri,
                                    std::vector<QrFloat3D> &triPos, std::vector<QrFloat2D> &triUv,
                                    std::vector<EditorEmissiveLight> &outLights)
{
    const EditorDynamicMeshDtalRequest &request = emit.request;
    const EditorMaterialParams &params = *request.params;
    for (int corner = 0; corner < 3; corner++)
    {
        const QrVertex &v = (*request.vertices)[(*request.indices)[tri.triIndex + size_t(corner)]];
        triPos[size_t(corner)] = { v.position[0], v.position[1], v.position[2] };
        triUv[size_t(corner)] = { v.texCoord[0], v.texCoord[1] };
    }

    EditorEmissiveLight light;
    light.cluster = request.cluster;
    light.isAliasModel = true;
    std::copy_n(tri.center, 3, light.center);
    QrTexturedAreaLightUploadInfo &info = light.info;
    info.uniqueID = request.nextUniqueId != nullptr ? ((*request.nextUniqueId)++)
                                                    : (kEditorFirstEmissiveLightUniqueID + outLights.size());
    info.color = { emit.colorPtr[0] * emit.gain, emit.colorPtr[1] * emit.gain,
                   emit.colorPtr[2] * emit.gain };
    info.normal = { tri.normal[0], tri.normal[1], tri.normal[2] };
    info.area = tri.area;
    info.meanEmiss = emit.masked ? request.emission.mean : 1.0f;
    info.material = emit.masked ? request.lightMaterial : QR_NO_MATERIAL;
    info.angleInner = emit.innerRad;
    info.angleOuter = emit.outerRad;
    info.projector = params.lightProjector ? 1.0f : 0.0f;
    info.isStatic = 0;
    info.lightShafts = params.lightShafts ? 1u : 0u;
    info.lightShaftsPower = std::max(0.0f, params.lightShaftsPower);
    info.lightShaftsDistance = std::max(0.0f, params.lightShaftsDistance);
    info.lightShaftsFalloff = std::max(0.01f, params.lightShaftsFalloff);

    float fitA[3] = {};
    float fitB[3] = {};
    float fitC[3] = {};
    if (FitFaceUvToPosition(triPos, triUv, fitA, fitB, fitC))
    {
        info.numVerts = 3;
        info.A = { fitA[0], fitA[1], fitA[2] };
        info.B = { fitB[0], fitB[1], fitB[2] };
        info.C = { fitC[0], fitC[1], fitC[2] };
        for (int corner = 0; corner < 3; corner++)
        {
            info.uvVerts[corner] = triUv[size_t(corner)];
        }
        info.fit = 1;
    }
    else if (BuildSquaredAreaLight(tri.center, tri.area, info))
    {
        info.fit = 1;
    }
    else
    {
        return false;
    }

    const float *c = info.color.data;
    light.power =
        info.area * info.meanEmiss * (0.2125f * c[0] + 0.7154f * c[1] + 0.0721f * c[2]);
    outLights.push_back(std::move(light));
    return true;
}

bool CanBuildDynamicMeshAreaLights(const EditorDynamicMeshDtalRequest &request, int32_t &outBudget,
                                   int32_t &outMaxPolys)
{
    if (request.settings == nullptr || request.params == nullptr || request.vertices == nullptr ||
        request.indices == nullptr)
    {
        return false;
    }
    const EditorRenderSettings &settings = *request.settings;
    const EditorMaterialParams &params = *request.params;
    if (!(settings.truelight > 0.0f) || !settings.modelLights || !params.enabled || !params.luma ||
        !(params.lightBrightness > 0.0f))
    {
        return false;
    }
    outBudget = request.budgetRemaining != nullptr
                    ? *request.budgetRemaining
                    : std::max<int32_t>(0, settings.dtalModelBudget);
    outMaxPolys = std::clamp(settings.dtalModelMaxPolys, 0, 32);
    return outBudget > 0 && outMaxPolys > 0;
}

} // namespace

void BuildEditorSceneEmissiveLights(const EditorScene &scene,
                                    const EditorMaterialTable &materials,
                                    const EditorRenderSettings &settings,
                                    const EditorDtalMaterialLookup &lookup,
                                    EditorDtalBuildResult &out)
{
    out = EditorDtalBuildResult{};
    std::vector<uint32_t> qualifiedAliasIds;
    std::vector<EditorGeneratedLightBase> proxyBases;

    if (settings.truelight > 0.0f)
    {
        const float globalFactor =
            (std::max(0.0f, settings.globalLightMult) * 0.1f) * settings.emissionLightIntensity *
            settings.brightness;
        const float intensityScale = kEditorEmissionLightScale * globalFactor;

        std::vector<AliasInstanceGroup> aliasGroups;
        std::unordered_map<uint32_t, size_t> aliasGroupIndex;
        SurfaceBuildContext ctx{
            scene, materials, settings, lookup, intensityScale, aliasGroups, aliasGroupIndex, out,
            proxyBases,
        };
        for (size_t surfaceIndex = 0; surfaceIndex < scene.surfaces.size(); surfaceIndex++)
        {
            CollectSurfaceForSceneEmissiveLights(ctx, surfaceIndex);
        }

        AppendCappedAliasInstanceLights(aliasGroups, settings.dtalModelBudget, out.emissiveLights,
                                        out.counts, qualifiedAliasIds);
    }

    CollectAliasBases(scene, materials, settings, qualifiedAliasIds, out.generatedLightBases);
    if (settings.truelight > 0.0f)
    {
        for (EditorGeneratedLightBase &proxy : proxyBases)
        {
            out.generatedLightBases.push_back(std::move(proxy));
        }
    }
}

QrResult UploadEditorEmissiveLights(QrInstance instance, const EditorScene &scene,
                                    const EditorMaterialTable &materials,
                                    const std::vector<EditorEmissiveLight> &emissiveLights,
                                    float timeSeconds,
                                    const std::vector<EditorEmissiveLight> *extraDynamicLights)
{
    const size_t staticCount = emissiveLights.size();
    const size_t extraCount = extraDynamicLights != nullptr ? extraDynamicLights->size() : 0u;
    if (instance == nullptr || (staticCount + extraCount) == 0u)
    {
        return QR_SUCCESS;
    }

    // Quake 3 q3map_surfacelight area lights stay steady while rgbGen wave shader stages animate
    // visually on surface overlays via UploadWorldOverlays; modulating area-light RGB with wave
    // stages drives lights below IsColorTooDim and causes 7-10 Hz room strobing and cluster churn.
    std::vector<QrTexturedAreaLightUploadInfo> infos;
    infos.reserve(staticCount + extraCount);
    for (const EditorEmissiveLight &entry : emissiveLights)
    {
        QrTexturedAreaLightUploadInfo info = entry.info;
        ApplySurfaceAnimatedTcModsToAreaLight(scene, materials, entry.surfaceIndex, timeSeconds,
                                              info);
        EditorMetersAreaLight(info);
        infos.push_back(info);
    }

    if (extraDynamicLights != nullptr)
    {
        for (const EditorEmissiveLight &dyn : *extraDynamicLights)
        {
            QrTexturedAreaLightUploadInfo info = dyn.info;
            EditorMetersAreaLight(info);
            infos.push_back(info);
        }
    }

    return qrUploadTexturedAreaLights(instance, infos.data(), uint32_t(infos.size()));
}

void BuildEditorDynamicMeshAreaLights(const EditorDynamicMeshDtalRequest &request,
                                      std::vector<EditorEmissiveLight> &outLights)
{
    int32_t budget = 0;
    int32_t maxPolys = 0;
    if (!CanBuildDynamicMeshAreaLights(request, budget, maxPolys))
    {
        return;
    }

    const std::vector<ModelTriRank> ranked =
        CollectRankedModelTriangles(*request.vertices, *request.indices);
    if (ranked.empty())
    {
        return;
    }

    const EditorRenderSettings &settings = *request.settings;
    const EditorMaterialParams &params = *request.params;
    const bool masked = request.emission.valid && request.emission.meanBase > 1.0e-6f;
    const float globalFactor =
        (std::max(0.0f, settings.globalLightMult) * 0.1f) * settings.emissionLightIntensity *
        settings.brightness;
    DynamicTriEmitParams emit{
        request,
        (params.hasLightColor || !masked) ? params.lightColor : request.emission.color,
        params.lightBrightness * kEditorEmissionLightScale * globalFactor,
        masked,
        0.0f,
        0.0f,
    };
    EmissiveCone(params.lightFocus, params.lightFocusSoft, params.lightProjector, emit.innerRad,
                 emit.outerRad);

    const int32_t take = std::min<int32_t>(int32_t(ranked.size()), std::min(maxPolys, budget));
    std::vector<QrFloat3D> triPos(3);
    std::vector<QrFloat2D> triUv(3);
    for (int32_t t = 0; t < take; t++)
    {
        if (AppendDynamicMeshTriangleLight(emit, ranked[size_t(t)], triPos, triUv, outLights))
        {
            budget--;
        }
    }

    if (request.budgetRemaining != nullptr)
    {
        *request.budgetRemaining = budget;
    }
}

const EditorEmitterLightOverride *FindEditorEmitterOverride(
    const std::vector<EditorEmitterLightOverride> &overrides, const std::string &name,
    uint64_t uniqueID)
{
    if (name.empty())
    {
        return nullptr;
    }

    std::vector<std::string> candidates;
    BuildEmitterOverrideCandidates(name, candidates);

    if (uniqueID != 0)
    {
        const std::string suffix = "#" + std::to_string(uniqueID);
        for (const std::string &base : candidates)
        {
            if (const EditorEmitterLightOverride *found =
                    FindEmitterOverrideExact(overrides, base + suffix))
            {
                return found;
            }
        }
    }

    for (const std::string &base : candidates)
    {
        if (const EditorEmitterLightOverride *found = FindEmitterOverrideExact(overrides, base))
        {
            return found;
        }
    }
    return nullptr;
}

EditorLight ResolveEditorSingleGeneratedLight(const EditorGeneratedLightBase &base,
                                              const EditorEmitterLightOverride *ov,
                                              const EditorRenderSettings &settings,
                                              const EditorScene &scene)
{
    EditorLight light;
    std::copy_n(base.position, 3, light.position);
    light.className = base.name;
    light.emitterUniqueID = base.uniqueID;
    light.modelSizeMeters = base.modelSizeMeters;

    ApplyGeneratedOverrideSpotAndOffset(base, ov, light);
    ApplyGeneratedOverrideRadiometry(base, ov, settings, light);

    const float offX = light.hasOffset ? light.offset[0] : 0.0f;
    const float offY = light.hasOffset ? light.offset[1] : 0.0f;
    const float offZ = light.hasOffset ? light.offset[2] : 0.0f;
    const float effectivePos[3] = {
        light.position[0] + offX,
        light.position[1] + offY,
        light.position[2] + offZ,
    };
    light.cluster = ResolveEditorLightCluster(scene, effectivePos);
    if (light.cluster == 0 || light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER)
    {
        light.cluster = ResolveEditorLightCluster(scene, light.position);
    }
    return light;
}
