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

#define MATERIAL_MAX_ALBEDO_LAYERS 0

#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#include "ShaderCommonHLSLFunc.hlsli"

#include "TurbWarp.hlsli"

struct HitAttributes
{
    float2 inBaryCoords;
};


#define ALPHA_THRESHOLD 0.5


[shader("anyhit")]
void main(inout ShPayload g_payload, in HitAttributes attribs)
{
	const ShTriangle tr = getTriangle((int)InstanceIndex(), (int)InstanceID(), (int)GeometryIndex(), (int)PrimitiveIndex());

	const bool isGlass = (tr.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0;
	const bool glassCutout = (tr.geometryInstanceFlags & GEOM_INST_FLAG_GLASS_CUTOUT) != 0;
	const bool shaderGlass = isGlass && globalUniform.glassBlur != 0u;
	const bool shaderReflection = shaderGlass && g_payload.glassFilter.y == 1.0 &&
		globalUniform.reflectRefractMaxDepth > 0u && isRegularPixOdd(int2(DispatchRaysIndex().xy)) == 0;

	/* Invisible entities share the glass pass-through; they never cut out or
	   tint. A plain pane accepts at once, without a fetch. */
	if (isGlass)
	{
		if ((tr.geometryInstanceFlags & GEOM_INST_FLAG_IGNORE_REFRACT_AFTER) != 0)
		{
			return;
		}
		if (!glassCutout && !shaderGlass)
		{
			return;
		}
	}

	const float3 baryCoords = float3(1.0f - attribs.inBaryCoords.x - attribs.inBaryCoords.y, attribs.inBaryCoords.x, attribs.inBaryCoords.y);
    const float2 texCoord = getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoords));

	if (!isGlass || glassCutout)
	{
		const float4 color = getTextureSampleLod(tr.materials[0][MATERIAL_ALBEDO_ALPHA_INDEX], texCoord, 0.0) * tr.materialColors[0];

		if ((tr.geometryInstanceFlags & GEOM_INST_FLAG_ALPHA_TRANSMISSION) != 0)
		{
			uint h = (uint)InstanceID() * 73856093u ^ (uint)PrimitiveIndex() * 19349663u ^ globalUniform.frameId * 83492791u;
			h ^= h >> 13;
			h *= 1274126177u;
			h ^= h >> 16;

			if (float(h) * (1.0 / 4294967296.0) < 1.0 - color.a)
			{
				IgnoreHit();
				return;
			}
		}
		else if ((color.r + color.g + color.b) / 3 * color.a + color.a < ALPHA_THRESHOLD)
		{
			IgnoreHit();
			return;
		}
	}

	if (shaderGlass)
	{
		if (g_payload.glassFilter.y == 1.0 && RayTCurrent() < g_payload.glassDistance)
		{
			const float3 tint = getTextureSampleLod(tr.materials[0][MATERIAL_ALBEDO_ALPHA_INDEX], texCoord, 0.0).rgb * tr.materialColors[0].rgb;
			float transparency = 1.0;
			if (tr.materials[0][MATERIAL_NORMAL_INDEX] != MATERIAL_NO_TEXTURE)
			{
				transparency = getTextureSampleLod(tr.materials[0][MATERIAL_NORMAL_INDEX], texCoord, 0.0).a;
			}
			float roughness = tr.geomRoughness;
			if (tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX] != MATERIAL_NO_TEXTURE)
			{
				roughness = getTextureSampleLod(tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX], texCoord, 0.0).r;
			}
			const float3 glassColor = tr.materialColors[1].xyz;
			g_payload.glassTint = glassTransmissionFilter(tint, glassColor, transparency);
			g_payload.glassDistance = RayTCurrent();
			g_payload.glassFilter.x = saturate(globalUniform.squareInputRoughness == 0u ? roughness : roughness * roughness);
			g_payload.glassFilter.z = 1.0;
			const float3 glassNormal = safeNormalize(cross(getColumn(tr.positions, 1) - getColumn(tr.positions, 0),
				getColumn(tr.positions, 2) - getColumn(tr.positions, 0)));
			const float ior = tr.materialColors[2].x > 0.0 ? clamp(tr.materialColors[2].x, 1.0, 5.0) : max(globalUniform.indexOfRefractionGlass, 1.0);
			const float f0 = square((ior - 1.0) / (ior + 1.0));
			g_payload.glassFilter.w = globalUniform.reflectRefractMaxDepth > 0u ?
				f0 + (1.0 - f0) * pow(1.0 - abs(dot(WorldRayDirection(), glassNormal)), 5.0) : 0.0;
		}
		if (shaderReflection)
		{
			return;
		}
		IgnoreHit();
		return;
	}
}
