// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/MemoryUtil.h"
#include "Core/Host.h"
#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/SplineCommon.h"

#include "GPU/Software/TransformUnit.h"
#include "GPU/Software/Clipper.h"
#include "GPU/Software/Lighting.h"

static u8 buf[65536 * 48];  // yolo
bool TransformUnit::outside_range_flag = false;

WorldCoords TransformUnit::ModelToWorld(const ModelCoords& coords)
{
	Mat3x3<float> world_matrix(gstate.worldMatrix);
	return WorldCoords(world_matrix * coords) + Vec3<float>(gstate.worldMatrix[9], gstate.worldMatrix[10], gstate.worldMatrix[11]);
}

WorldCoords TransformUnit::ModelToWorldNormal(const ModelCoords& coords)
{
	Mat3x3<float> world_matrix(gstate.worldMatrix);
	return WorldCoords(world_matrix * coords);
}

ViewCoords TransformUnit::WorldToView(const WorldCoords& coords)
{
	Mat3x3<float> view_matrix(gstate.viewMatrix);
	return ViewCoords(view_matrix * coords) + Vec3<float>(gstate.viewMatrix[9], gstate.viewMatrix[10], gstate.viewMatrix[11]);
}

ClipCoords TransformUnit::ViewToClip(const ViewCoords& coords)
{
	Vec4<float> coords4(coords.x, coords.y, coords.z, 1.0f);
	Mat4x4<float> projection_matrix(gstate.projMatrix);
	return ClipCoords(projection_matrix * coords4);
}

static inline ScreenCoords ClipToScreenInternal(const ClipCoords& coords, bool *outside_range_flag) {
	ScreenCoords ret;

	// Parameters here can seem invalid, but the PSP is fine with negative viewport widths etc.
	// The checking that OpenGL and D3D do is actually quite superflous as the calculations still "work"
	// with some pretty crazy inputs, which PSP games are happy to do at times.
	float xScale = gstate.getViewportXScale();
	float xCenter = gstate.getViewportXCenter();
	float yScale = gstate.getViewportYScale();
	float yCenter = gstate.getViewportYCenter();
	float zScale = gstate.getViewportZScale();
	float zCenter = gstate.getViewportZCenter();

	float x = coords.x * xScale / coords.w + xCenter;
	float y = coords.y * yScale / coords.w + yCenter;
	float z = coords.z * zScale / coords.w + zCenter;

	// Is this really right?
	if (gstate.clipEnable & 0x1) {
		if (z < 0.f)
			z = 0.f;
		if (z > 65535.f)
			z = 65535.f;
	}

	if (outside_range_flag && (x > 4095.9375f || y > 4095.9375f || x < 0 || y < 0 || z < 0 || z > 65535.f))
		*outside_range_flag = true;

	// 16 = 0xFFFF / 4095.9375
	// Round up at 0.625 to the nearest subpixel.
	return ScreenCoords(x * 16.0f + 0.375f, y * 16.0f + 0.375f, z);
}

ScreenCoords TransformUnit::ClipToScreen(const ClipCoords& coords)
{
	return ClipToScreenInternal(coords, nullptr);
}

DrawingCoords TransformUnit::ScreenToDrawing(const ScreenCoords& coords)
{
	DrawingCoords ret;
	// TODO: What to do when offset > coord?
	ret.x = (((u32)coords.x - gstate.getOffsetX16()) / 16) & 0x3ff;
	ret.y = (((u32)coords.y - gstate.getOffsetY16()) / 16) & 0x3ff;
	ret.z = coords.z;
	return ret;
}

ScreenCoords TransformUnit::DrawingToScreen(const DrawingCoords& coords)
{
	ScreenCoords ret;
	ret.x = (u32)coords.x * 16 + gstate.getOffsetX16();
	ret.y = (u32)coords.y * 16 + gstate.getOffsetY16();
	ret.z = coords.z;
	return ret;
}

VertexData TransformUnit::ReadVertex(VertexReader& vreader)
{
	VertexData vertex;

	float pos[3];
	// VertexDecoder normally scales z, but we want it unscaled.
	vreader.ReadPosThroughZ16(pos);

	if (!gstate.isModeClear() && gstate.isTextureMapEnabled() && vreader.hasUV()) {
		float uv[2];
		vreader.ReadUV(uv);
		vertex.texturecoords = Vec2<float>(uv[0], uv[1]);
	}

	if (vreader.hasNormal()) {
		float normal[3];
		vreader.ReadNrm(normal);
		vertex.normal = Vec3<float>(normal[0], normal[1], normal[2]);

		if (gstate.areNormalsReversed())
			vertex.normal = -vertex.normal;
	}

	if (vertTypeIsSkinningEnabled(gstate.vertType) && !gstate.isModeThrough()) {
		float W[8] = { 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
		vreader.ReadWeights(W);

		Vec3<float> tmppos(0.f, 0.f, 0.f);
		Vec3<float> tmpnrm(0.f, 0.f, 0.f);

		for (int i = 0; i < vertTypeGetNumBoneWeights(gstate.vertType); ++i) {
			Mat3x3<float> bone(&gstate.boneMatrix[12*i]);
			tmppos += (bone * ModelCoords(pos[0], pos[1], pos[2]) + Vec3<float>(gstate.boneMatrix[12*i+9], gstate.boneMatrix[12*i+10], gstate.boneMatrix[12*i+11])) * W[i];
			if (vreader.hasNormal())
				tmpnrm += (bone * vertex.normal) * W[i];
		}

		pos[0] = tmppos.x;
		pos[1] = tmppos.y;
		pos[2] = tmppos.z;
		if (vreader.hasNormal())
			vertex.normal = tmpnrm;
	}

	if (vreader.hasColor0()) {
		float col[4];
		vreader.ReadColor0(col);
		vertex.color0 = Vec4<int>(col[0]*255, col[1]*255, col[2]*255, col[3]*255);
	} else {
		vertex.color0 = Vec4<int>(gstate.getMaterialAmbientR(), gstate.getMaterialAmbientG(), gstate.getMaterialAmbientB(), gstate.getMaterialAmbientA());
	}

	if (vreader.hasColor1()) {
		float col[3];
		vreader.ReadColor1(col);
		vertex.color1 = Vec3<int>(col[0]*255, col[1]*255, col[2]*255);
	} else {
		vertex.color1 = Vec3<int>(0, 0, 0);
	}

	if (!gstate.isModeThrough()) {
		vertex.modelpos = ModelCoords(pos[0], pos[1], pos[2]);
		vertex.worldpos = WorldCoords(TransformUnit::ModelToWorld(vertex.modelpos));
		ModelCoords viewpos = TransformUnit::WorldToView(vertex.worldpos);
		vertex.clippos = ClipCoords(TransformUnit::ViewToClip(viewpos));
		if (gstate.isFogEnabled()) {
			vertex.fogdepth = (viewpos.z + getFloat24(gstate.fog1)) * getFloat24(gstate.fog2);
		} else {
			vertex.fogdepth = 1.0f;
		}
		vertex.screenpos = ClipToScreenInternal(vertex.clippos, &outside_range_flag);

		if (vreader.hasNormal()) {
			vertex.worldnormal = TransformUnit::ModelToWorldNormal(vertex.normal);
			// TODO: Isn't there a flag that controls whether to normalize the normal?
			vertex.worldnormal /= vertex.worldnormal.Length();
		} else {
			vertex.worldnormal = Vec3<float>(0.0f, 0.0f, 1.0f);
		}

		Lighting::Process(vertex, vreader.hasColor0());
	} else {
		vertex.screenpos.x = (int)(pos[0] * 16) + gstate.getOffsetX16();
		vertex.screenpos.y = (int)(pos[1] * 16) + gstate.getOffsetY16();
		vertex.screenpos.z = pos[2];
		vertex.clippos.w = 1.f;
		vertex.fogdepth = 1.f;
	}

	return vertex;
}

#define START_OPEN_U 1
#define END_OPEN_U 2
#define START_OPEN_V 4
#define END_OPEN_V 8

struct SplinePatch {
	VertexData points[16];
	int type;
	int pad[3];
};

void TransformUnit::SubmitPrimitive(void* vertices, void* indices, GEPrimitiveType prim_type, int vertex_count, u32 vertex_type, int *bytesRead)
{
	// TODO: Cache VertexDecoder objects
	VertexDecoder vdecoder;
	VertexDecoderOptions options{};
	vdecoder.SetVertexType(vertex_type, options);
	const DecVtxFormat& vtxfmt = vdecoder.GetDecVtxFmt();

	if (bytesRead)
		*bytesRead = vertex_count * vdecoder.VertexSize();

	// Frame skipping.
	if (gstate_c.skipDrawReason & SKIPDRAW_SKIPFRAME) {
		return;
	}

	u16 index_lower_bound = 0;
	u16 index_upper_bound = vertex_count - 1;
	IndexConverter idxConv(vertex_type, indices);

	if (indices)
		GetIndexBounds(indices, vertex_count, vertex_type, &index_lower_bound, &index_upper_bound);
	vdecoder.DecodeVerts(buf, vertices, index_lower_bound, index_upper_bound);

	VertexReader vreader(buf, vtxfmt, vertex_type);

	const int max_vtcs_per_prim = 3;
	static VertexData data[max_vtcs_per_prim];
	// This is the index of the next vert in data (or higher, may need modulus.)
	static int data_index = 0;

	static GEPrimitiveType prev_prim = GE_PRIM_POINTS;
	if (prim_type != GE_PRIM_KEEP_PREVIOUS) {
		data_index = 0;
		prev_prim = prim_type;
	} else {
		prim_type = prev_prim;
	}

	int vtcs_per_prim;
	switch (prim_type) {
	case GE_PRIM_POINTS: vtcs_per_prim = 1; break;
	case GE_PRIM_LINES: vtcs_per_prim = 2; break;
	case GE_PRIM_TRIANGLES: vtcs_per_prim = 3; break;
	case GE_PRIM_RECTANGLES: vtcs_per_prim = 2; break;
	default: vtcs_per_prim = 0; break;
	}

	// TODO: Do this in two passes - first process the vertices (before indexing/stripping),
	// then resolve the indices. This lets us avoid transforming shared vertices twice.

	switch (prim_type) {
	case GE_PRIM_POINTS:
	case GE_PRIM_LINES:
	case GE_PRIM_TRIANGLES:
	case GE_PRIM_RECTANGLES:
		{
			for (int vtx = 0; vtx < vertex_count; ++vtx) {
				if (indices) {
					vreader.Goto(idxConv.convert(vtx) - index_lower_bound);
				} else {
					vreader.Goto(vtx);
				}

				data[data_index++] = ReadVertex(vreader);
				if (data_index < vtcs_per_prim) {
					// Keep reading.  Note: an incomplete prim will stay read for GE_PRIM_KEEP_PREVIOUS.
					continue;
				}

				// Okay, we've got enough verts.  Reset the index for next time.
				data_index = 0;
				if (outside_range_flag) {
					// Cull the prim if it was outside, and move to the next prim.
					outside_range_flag = false;
					continue;
				}

				switch (prim_type) {
				case GE_PRIM_TRIANGLES:
				{
					if (!gstate.isCullEnabled() || gstate.isModeClear()) {
						Clipper::ProcessTriangle(data[0], data[1], data[2]);
						Clipper::ProcessTriangle(data[2], data[1], data[0]);
					} else if (!gstate.getCullMode()) {
						Clipper::ProcessTriangle(data[2], data[1], data[0]);
					} else {
						Clipper::ProcessTriangle(data[0], data[1], data[2]);
					}
					break;
				}

				case GE_PRIM_RECTANGLES:
					Clipper::ProcessRect(data[0], data[1]);
					break;

				case GE_PRIM_LINES:
					Clipper::ProcessLine(data[0], data[1]);
					break;

				case GE_PRIM_POINTS:
					Clipper::ProcessPoint(data[0]);
					break;
				}
			}
			break;
		}

	case GE_PRIM_LINE_STRIP:
		{
			// Don't draw a line when loading the first vertex.
			// If data_index is 1 or 2, etc., it means we're continuing a line strip.
			int skip_count = data_index == 0 ? 1 : 0;
			for (int vtx = 0; vtx < vertex_count; ++vtx) {
				if (indices) {
					vreader.Goto(idxConv.convert(vtx) - index_lower_bound);
				} else {
					vreader.Goto(vtx);
				}

				data[(data_index++) & 1] = ReadVertex(vreader);
				if (outside_range_flag) {
					// Drop all primitives containing the current vertex
					skip_count = 2;
					outside_range_flag = false;
					continue;
				}

				if (skip_count) {
					--skip_count;
				} else {
					// We already incremented data_index, so data_index & 1 is previous one.
					Clipper::ProcessLine(data[data_index & 1], data[(data_index & 1) ^ 1]);
				}
			}
			break;
		}

	case GE_PRIM_TRIANGLE_STRIP:
		{
			// Don't draw a triangle when loading the first two vertices.
			int skip_count = data_index >= 2 ? 0 : 2 - data_index;

			for (int vtx = 0; vtx < vertex_count; ++vtx) {
				if (indices) {
					vreader.Goto(idxConv.convert(vtx) - index_lower_bound);
				} else {
					vreader.Goto(vtx);
				}

				data[(data_index++) % 3] = ReadVertex(vreader);
				if (outside_range_flag) {
					// Drop all primitives containing the current vertex
					skip_count = 2;
					outside_range_flag = false;
					continue;
				}

				if (skip_count) {
					--skip_count;
					continue;
				}

				if (!gstate.isCullEnabled() || gstate.isModeClear()) {
					Clipper::ProcessTriangle(data[0], data[1], data[2]);
					Clipper::ProcessTriangle(data[2], data[1], data[0]);
				} else if ((!gstate.getCullMode()) ^ ((data_index - 1) % 2)) {
					// We need to reverse the vertex order for each second primitive,
					// but we additionally need to do that for every primitive if CCW cullmode is used.
					Clipper::ProcessTriangle(data[2], data[1], data[0]);
				} else {
					Clipper::ProcessTriangle(data[0], data[1], data[2]);
				}
			}
			break;
		}

	case GE_PRIM_TRIANGLE_FAN:
		{
			// Don't draw a triangle when loading the first two vertices.
			// (this doesn't count the central one.)
			int skip_count = data_index <= 1 ? 1 : 0;
			int start_vtx = 0;

			// Only read the central vertex if we're not continuing.
			if (data_index == 0) {
				if (indices) {
					vreader.Goto(idxConv.convert(0) - index_lower_bound);
				} else {
					vreader.Goto(0);
				}
				data[0] = ReadVertex(vreader);
				data_index++;
				start_vtx = 1;
			}

			for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
				if (indices) {
					vreader.Goto(idxConv.convert(vtx) - index_lower_bound);
				} else {
					vreader.Goto(vtx);
				}

				data[2 - ((data_index++) % 2)] = ReadVertex(vreader);
				if (outside_range_flag) {
					// Drop all primitives containing the current vertex
					skip_count = 2;
					outside_range_flag = false;
					continue;
				}

				if (skip_count) {
					--skip_count;
					continue;
				}

				if (!gstate.isCullEnabled() || gstate.isModeClear()) {
					Clipper::ProcessTriangle(data[0], data[1], data[2]);
					Clipper::ProcessTriangle(data[2], data[1], data[0]);
				} else if ((!gstate.getCullMode()) ^ ((data_index - 1) % 2)) {
					// We need to reverse the vertex order for each second primitive,
					// but we additionally need to do that for every primitive if CCW cullmode is used.
					Clipper::ProcessTriangle(data[2], data[1], data[0]);
				} else {
					Clipper::ProcessTriangle(data[0], data[1], data[2]);
				}
			}
			break;
		}
	}

	host->GPUNotifyDraw();
}

// TODO: This probably is not the best interface.
bool TransformUnit::GetCurrentSimpleVertices(int count, std::vector<GPUDebugVertex> &vertices, std::vector<u16> &indices) {
	// This is always for the current vertices.
	u16 indexLowerBound = 0;
	u16 indexUpperBound = count - 1;

	if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		const u8 *inds = Memory::GetPointer(gstate_c.indexAddr);
		const u16 *inds16 = (const u16 *)inds;
		const u32 *inds32 = (const u32 *)inds;

		if (inds) {
			GetIndexBounds(inds, count, gstate.vertType, &indexLowerBound, &indexUpperBound);
			indices.resize(count);
			switch (gstate.vertType & GE_VTYPE_IDX_MASK) {
			case GE_VTYPE_IDX_8BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds[i];
				}
				break;
			case GE_VTYPE_IDX_16BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds16[i];
				}
				break;
			case GE_VTYPE_IDX_32BIT:
				WARN_LOG_REPORT_ONCE(simpleIndexes32, G3D, "SimpleVertices: Decoding 32-bit indexes");
				for (int i = 0; i < count; ++i) {
					// These aren't documented and should be rare.  Let's bounds check each one.
					if (inds32[i] != (u16)inds32[i]) {
						ERROR_LOG_REPORT_ONCE(simpleIndexes32Bounds, G3D, "SimpleVertices: Index outside 16-bit range");
					}
					indices[i] = (u16)inds32[i];
				}
				break;
			}
		} else {
			indices.clear();
		}
	} else {
		indices.clear();
	}

	static std::vector<u32> temp_buffer;
	static std::vector<SimpleVertex> simpleVertices;
	temp_buffer.resize(65536 * 24 / sizeof(u32));
	simpleVertices.resize(indexUpperBound + 1);

	VertexDecoder vdecoder;
	VertexDecoderOptions options{};
	vdecoder.SetVertexType(gstate.vertType, options);
	DrawEngineCommon::NormalizeVertices((u8 *)(&simpleVertices[0]), (u8 *)(&temp_buffer[0]), Memory::GetPointer(gstate_c.vertexAddr), &vdecoder, indexLowerBound, indexUpperBound, gstate.vertType);

	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);

	vertices.resize(indexUpperBound + 1);
	for (int i = indexLowerBound; i <= indexUpperBound; ++i) {
		const SimpleVertex &vert = simpleVertices[i];

		if (gstate.isModeThrough()) {
			if (gstate.vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0];
				vertices[i].v = vert.uv[1];
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			vertices[i].x = vert.pos.x;
			vertices[i].y = vert.pos.y;
			vertices[i].z = vert.pos.z;
			if (gstate.vertType & GE_VTYPE_COL_MASK) {
				memcpy(vertices[i].c, vert.color, sizeof(vertices[i].c));
			} else {
				memset(vertices[i].c, 0, sizeof(vertices[i].c));
			}
		} else {
			float clipPos[4];
			Vec3ByMatrix44(clipPos, vert.pos.AsArray(), worldviewproj);
			ScreenCoords screenPos = ClipToScreen(clipPos);
			DrawingCoords drawPos = ScreenToDrawing(screenPos);

			if (gstate.vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0];
				vertices[i].v = vert.uv[1];
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			vertices[i].x = drawPos.x;
			vertices[i].y = drawPos.y;
			vertices[i].z = 1.0;
			if (gstate.vertType & GE_VTYPE_COL_MASK) {
				memcpy(vertices[i].c, vert.color, sizeof(vertices[i].c));
			} else {
				memset(vertices[i].c, 0, sizeof(vertices[i].c));
			}
		}
	}

	return true;
}
