#include "EditorViewport.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace
{

constexpr float kOutlineNearClip = 1.0f;
constexpr float kOutlineNudgeUnits = 0.35f;
constexpr size_t kOutlineMaxVerts = 256;
constexpr int kMaxEmissivePolysPerFace = 64;

bool RayTriangleIntersect(const float origin[3], const float direction[3], const float a[3],
                          const float b[3], const float c[3], float &outDistance)
{
    const float edge1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float edge2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

    const float pvec[3] = {
        direction[1] * edge2[2] - direction[2] * edge2[1],
        direction[2] * edge2[0] - direction[0] * edge2[2],
        direction[0] * edge2[1] - direction[1] * edge2[0],
    };

    const float det = edge1[0] * pvec[0] + edge1[1] * pvec[1] + edge1[2] * pvec[2];

    if (std::fabs(det) < 1e-9f)
    {
        return false;
    }

    const float inv = 1.0f / det;
    const float tvec[3] = { origin[0] - a[0], origin[1] - a[1], origin[2] - a[2] };
    const float u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * inv;

    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }

    const float qvec[3] = {
        tvec[1] * edge1[2] - tvec[2] * edge1[1],
        tvec[2] * edge1[0] - tvec[0] * edge1[2],
        tvec[0] * edge1[1] - tvec[1] * edge1[0],
    };

    const float v = (direction[0] * qvec[0] + direction[1] * qvec[1] + direction[2] * qvec[2]) * inv;

    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }

    const float dist = (edge2[0] * qvec[0] + edge2[1] * qvec[1] + edge2[2] * qvec[2]) * inv;

    if (dist <= 0.0f)
    {
        return false;
    }

    outDistance = dist;
    return true;
}

bool PickFaceFan(const float origin[3], const float direction[3], const EditorSurfaceFace &face,
                 float &outDistance)
{
    if (face.positions.size() < 3)
    {
        return false;
    }

    bool hit = false;
    float best = 0.0f;
    const float *hub = face.positions[0].data;

    for (size_t k = 1; k + 1 < face.positions.size(); k++)
    {
        float dist = 0.0f;

        if (!RayTriangleIntersect(origin, direction, hub, face.positions[k].data,
                                  face.positions[k + 1].data, dist))
        {
            continue;
        }

        if (!hit || dist < best)
        {
            hit = true;
            best = dist;
        }
    }

    if (hit)
    {
        outDistance = best;
    }

    return hit;
}

bool PickSurfaceTrianglesFallback(const float origin[3], const float direction[3],
                                  const EditorSurface &surface, float &outDistance,
                                  uint32_t &outCluster)
{
    bool hit = false;
    float best = 0.0f;

    for (size_t i = 0; i + 2 < surface.indices.size(); i += 3)
    {
        const uint32_t ia = surface.indices[i + 0];
        const uint32_t ib = surface.indices[i + 1];
        const uint32_t ic = surface.indices[i + 2];

        if (ia >= surface.vertices.size() || ib >= surface.vertices.size() ||
            ic >= surface.vertices.size())
        {
            continue;
        }

        float dist = 0.0f;

        if (!RayTriangleIntersect(origin, direction, surface.vertices[ia].position,
                                  surface.vertices[ib].position, surface.vertices[ic].position,
                                  dist))
        {
            continue;
        }

        if (!hit || dist < best)
        {
            hit = true;
            best = dist;
            outCluster = surface.vertices[ia].cluster;
        }
    }

    if (hit)
    {
        outDistance = best;
    }

    return hit;
}

float Dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
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

} // namespace

bool ProjectClippedEdge(const EditorCameraBasis &cam, const float a[3], const float b[3],
                        float outSeg[4])
{
    if (cam.tanHalfFovX <= 0.0f || cam.tanHalfFovY <= 0.0f || cam.viewportWidth <= 0.0f ||
        cam.viewportHeight <= 0.0f)
    {
        return false;
    }

    const float da[3] = { a[0] - cam.origin[0], a[1] - cam.origin[1], a[2] - cam.origin[2] };
    const float db[3] = { b[0] - cam.origin[0], b[1] - cam.origin[1], b[2] - cam.origin[2] };

    float za = Dot3(da, cam.forward);
    float zb = Dot3(db, cam.forward);

    if (za <= kOutlineNearClip && zb <= kOutlineNearClip)
    {
        return false;
    }

    float xa = Dot3(da, cam.right);
    float ya = Dot3(da, cam.up);
    float xb = Dot3(db, cam.right);
    float yb = Dot3(db, cam.up);

    if (za < kOutlineNearClip)
    {
        const float t = (kOutlineNearClip - za) / (zb - za);
        xa += t * (xb - xa);
        ya += t * (yb - ya);
        za = kOutlineNearClip;
    }
    else if (zb < kOutlineNearClip)
    {
        const float t = (kOutlineNearClip - za) / (zb - za);
        xb = xa + t * (xb - xa);
        yb = ya + t * (yb - ya);
        zb = kOutlineNearClip;
    }

    outSeg[0] = (0.5f + 0.5f * (xa / za) / cam.tanHalfFovX) * cam.viewportWidth;
    outSeg[1] = (0.5f - 0.5f * (ya / za) / cam.tanHalfFovY) * cam.viewportHeight;
    outSeg[2] = (0.5f + 0.5f * (xb / zb) / cam.tanHalfFovX) * cam.viewportWidth;
    outSeg[3] = (0.5f - 0.5f * (yb / zb) / cam.tanHalfFovY) * cam.viewportHeight;
    return true;
}

void EmitClippedPolygonOutline(ImDrawList *drawList, const EditorCameraBasis &cam,
                               const EditorSurfaceFace &face, uint32_t color, float thickness)
{
    if (drawList == nullptr || face.positions.size() < 3)
    {
        return;
    }

    const size_t n = std::min(face.positions.size(), kOutlineMaxVerts);
    const float toView[3] = {
        cam.origin[0] - face.positions[0].data[0],
        cam.origin[1] - face.positions[0].data[1],
        cam.origin[2] - face.positions[0].data[2],
    };
    const float nudge = Dot3(face.normal, toView) < 0.0f ? -kOutlineNudgeUnits : kOutlineNudgeUnits;

    std::vector<QrFloat3D> world(n);

    for (size_t i = 0; i < n; i++)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            world[i].data[axis] = face.positions[i].data[axis] + face.normal[axis] * nudge;
        }
    }

    for (size_t i = 0; i < n; i++)
    {
        float seg[4] = {};

        if (ProjectClippedEdge(cam, world[i].data, world[(i + 1) % n].data, seg))
        {
            drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]), ImU32(color),
                              thickness);
        }
    }
}

EditorFacePick PickSceneFace(const EditorScene &scene, const float origin[3],
                             const float direction[3])
{
    EditorFacePick best;

    for (size_t s = 0; s < scene.surfaces.size(); s++)
    {
        const EditorSurface &surface = scene.surfaces[s];

        if (surface.faces.empty())
        {
            float dist = 0.0f;
            uint32_t cluster = 0;

            if (PickSurfaceTrianglesFallback(origin, direction, surface, dist, cluster) &&
                (best.surface < 0 || dist < best.distance))
            {
                best = { int32_t(s), -1, dist, {}, cluster };
            }

            continue;
        }

        for (size_t f = 0; f < surface.faces.size(); f++)
        {
            float dist = 0.0f;

            if (PickFaceFan(origin, direction, surface.faces[f], dist) &&
                (best.surface < 0 || dist < best.distance))
            {
                best = { int32_t(s), int32_t(f), dist, {}, surface.faces[f].cluster };
            }
        }
    }

    if (best.surface >= 0)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            best.point[axis] = origin[axis] + direction[axis] * best.distance;
        }
    }

    return best;
}

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

    if (tileSpanX <= 0.0 || tileSpanY <= 0.0 || tileSpanX * tileSpanY > double(kMaxEmissivePolysPerFace))
    {
        return false;
    }

    const float cross[3] = {
        A[1] * B[2] - A[2] * B[1],
        A[2] * B[0] - A[0] * B[2],
        A[0] * B[1] - A[1] * B[0],
    };
    const float jacobian = std::sqrt(Dot3(cross, cross));

    for (int ty = tileMin[1]; ty <= tileMax[1]; ty++)
    {
        for (int tx = tileMin[0]; tx <= tileMax[0]; tx++)
        {
            const float uvMinTile[2] = { float(tx) + emission.glowUvMin[0],
                                         float(ty) + emission.glowUvMin[1] };
            const float uvMaxTile[2] = { float(tx) + emission.glowUvMax[0],
                                         float(ty) + emission.glowUvMax[1] };

            EditorClippedGlowPoly poly;
            poly.numVerts =
                ClipUvPolyToRect(surfUv.data(), int(surfUv.size()), uvMinTile, uvMaxTile, poly.uvVerts);

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
