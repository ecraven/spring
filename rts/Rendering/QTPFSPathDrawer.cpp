#include <limits>

#include "Game/Camera.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"

#include "Sim/Path/QTPFS/Path.hpp"
#include "Sim/Path/QTPFS/Node.hpp"
#include "Sim/Path/QTPFS/NodeLayer.hpp"
#include "Sim/Path/QTPFS/PathCache.hpp"
#include "Sim/Path/QTPFS/PathManager.hpp"

#include "Rendering/Fonts/glFont.h"
#include "Rendering/QTPFSPathDrawer.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/VertexArray.h"
#include "System/StringUtil.h"

QTPFSPathDrawer::QTPFSPathDrawer(): IPathDrawer() {
	pm = dynamic_cast<QTPFS::PathManager*>(pathManager);
}

void QTPFSPathDrawer::DrawAll() const {
	const MoveDef* md = GetSelectedMoveDef();

	if (md == nullptr)
		return;

	if (!enabled)
		return;

	if (!gs->cheatEnabled && !gu->spectating)
		return;

	glPushAttrib(GL_ENABLE_BIT | GL_POLYGON_BIT);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);

	DrawNodeTree(md);
	DrawPaths(md);

	glPopAttrib();
}

void QTPFSPathDrawer::DrawNodeTree(const MoveDef* md) const {
	CVertexArray* va = GetVertexArray();

	std::vector<const QTPFS::QTNode*> nodes;
	std::vector<const QTPFS::QTNode*>::const_iterator nodesIt;

	GetVisibleNodes(pm->GetNodeTree(md->pathType), pm->GetNodeLayer(md->pathType), nodes);

	va->Initialize();
	va->EnlargeArrays(nodes.size() * 4, 0, VA_SIZE_C);

	for (nodesIt = nodes.begin(); nodesIt != nodes.end(); ++nodesIt) {
		DrawNode(*nodesIt, md, va, false, true, true);
	}

	glLineWidth(2);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	va->DrawArrayC(GL_QUADS);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glLineWidth(1);
}



void QTPFSPathDrawer::GetVisibleNodes(const QTPFS::QTNode* nt, const QTPFS::NodeLayer& nl, std::vector<const QTPFS::QTNode*>& nodes) const {
	if (nt->IsLeaf()) {
		nodes.push_back(nt);
		return;
	}

	for (unsigned int i = 0; i < QTNODE_CHILD_COUNT; i++) {
		const QTPFS::QTNode* cn = nl.GetPoolNode(nt->GetChildBaseIndex() + i);
		const float3 mins = float3(cn->xmin() * SQUARE_SIZE, 0.0f, cn->zmin() * SQUARE_SIZE);
		const float3 maxs = float3(cn->xmax() * SQUARE_SIZE, 0.0f, cn->zmax() * SQUARE_SIZE);

		if (!camera->InView(mins, maxs))
			continue;

		GetVisibleNodes(cn, nl, nodes);
	}
}



void QTPFSPathDrawer::DrawPaths(const MoveDef* md) const {
	const QTPFS::PathCache& pathCache = pm->GetPathCache(md->pathType);
	const QTPFS::PathCache::PathMap& paths = pathCache.GetLivePaths();

	#ifdef QTPFS_TRACE_PATH_SEARCHES
	const auto& pathTypes = pm->GetPathTypes();
	const auto& pathTraces = pm->GetPathTraces();
	#endif

	QTPFS::PathCache::PathMap::const_iterator pathsIt;

	CVertexArray* va = GetVertexArray();

	for (pathsIt = paths.begin(); pathsIt != paths.end(); ++pathsIt) {
		DrawPath(pathsIt->second, va);

		#ifdef QTPFS_TRACE_PATH_SEARCHES
		#define PM QTPFS::PathManager
		const PM::PathTypeMap::const_iterator typeIt = pathTypes.find(pathsIt->first);
		const PM::PathTraceMap::const_iterator traceIt = pathTraces.find(pathsIt->first);
		#undef PM

		if (typeIt == pathTypes.end() || traceIt == pathTraces.end())
			continue;
		// this only happens if source-node was equal to target-node
		if (traceIt->second == nullptr)
			continue;

		DrawSearchExecution(typeIt->second, traceIt->second);
		#endif
	}
}

void QTPFSPathDrawer::DrawPath(const QTPFS::IPath* path, CVertexArray* va) const {
	glLineWidth(4);

	{
		va->Initialize();
		va->EnlargeArrays(path->NumPoints() * 2, 0, VA_SIZE_C);

		static constexpr unsigned char color[4] = {
			0 * 255, 0 * 255, 1 * 255, 1 * 255,
		};

		for (unsigned int n = 0; n < path->NumPoints() - 1; n++) {
			float3 p0 = path->GetPoint(n + 0);
			float3 p1 = path->GetPoint(n + 1);

			if (!camera->InView(p0) && !camera->InView(p1))
				continue;

			p0.y = CGround::GetHeightReal(p0.x, p0.z, false);
			p1.y = CGround::GetHeightReal(p1.x, p1.z, false);

			va->AddVertexQC(p0, color);
			va->AddVertexQC(p1, color);
		}

		va->DrawArrayC(GL_LINES);
	}

	#ifdef QTPFS_DRAW_WAYPOINT_GROUND_CIRCLES
	{
		glColor4ub(color[0], color[1], color[2], color[3]);

		for (unsigned int n = 0; n < path->NumPoints(); n++) {
			glSurfaceCircle(path->GetPoint(n), path->GetRadius(), 16);
		}

		glColor4ub(255, 255, 255, 255);
	}
	#endif

	glLineWidth(1);
}

void QTPFSPathDrawer::DrawSearchExecution(unsigned int pathType, const QTPFS::PathSearchTrace::Execution* searchExec) const {
	// TODO: prevent overdraw so oft-visited nodes do not become darker
	const std::vector<QTPFS::PathSearchTrace::Iteration>& searchIters = searchExec->GetIterations();
	      std::vector<QTPFS::PathSearchTrace::Iteration>::const_iterator it;
	const unsigned searchFrame = searchExec->GetFrame();

	// number of frames reserved to draw the entire trace
	static const unsigned int MAX_DRAW_TIME = GAME_SPEED * 5;

	unsigned int itersPerFrame = (searchIters.size() / MAX_DRAW_TIME) + 1;
	unsigned int curSearchIter = 0;
	unsigned int maxSearchIter = ((gs->frameNum - searchFrame) + 1) * itersPerFrame;

	CVertexArray* va = GetVertexArray();

	for (it = searchIters.begin(); it != searchIters.end(); ++it) {
		if (curSearchIter++ >= maxSearchIter)
			break;

		const QTPFS::PathSearchTrace::Iteration& searchIter = *it;
		const std::vector<unsigned int>& nodeIndices = searchIter.GetNodeIndices();

		DrawSearchIteration(pathType, nodeIndices, va);
	}
}

void QTPFSPathDrawer::DrawSearchIteration(unsigned int pathType, const std::vector<unsigned int>& nodeIndices, CVertexArray* va) const {
	std::vector<unsigned int>::const_iterator it = nodeIndices.cbegin();

	unsigned int hmx = (*it) % mapDims.mapx;
	unsigned int hmz = (*it) / mapDims.mapx;

	const QTPFS::NodeLayer& nodeLayer = pm->GetNodeLayer(pathType);
	const QTPFS::QTNode* poppedNode = static_cast<const QTPFS::QTNode*>(nodeLayer.GetNode(hmx, hmz));
	const QTPFS::QTNode* pushedNode = nullptr;

	DrawNode(poppedNode, nullptr, va, true, false, false);

	for (++it; it != nodeIndices.end(); ++it) {
		hmx = (*it) % mapDims.mapx;
		hmz = (*it) / mapDims.mapx;

		pushedNode = static_cast<const QTPFS::QTNode*>(nodeLayer.GetNode(hmx, hmz));

		DrawNode(pushedNode, nullptr, va, true, false, false);
		DrawNodeLink(pushedNode, poppedNode, va);
	}
}

void QTPFSPathDrawer::DrawNode(
	const QTPFS::QTNode* node,
	const MoveDef* md,
	CVertexArray* va,
	bool fillQuad,
	bool showCost,
	bool batchDraw
) const {
	#define xminw (node->xmin() * SQUARE_SIZE)
	#define xmaxw (node->xmax() * SQUARE_SIZE)
	#define zminw (node->zmin() * SQUARE_SIZE)
	#define zmaxw (node->zmax() * SQUARE_SIZE)
	#define xmidw (node->xmid() * SQUARE_SIZE)
	#define zmidw (node->zmid() * SQUARE_SIZE)

	const float3 verts[5] = {
		float3(xminw, CGround::GetHeightReal(xminw, zminw, false) + 4.0f, zminw),
		float3(xmaxw, CGround::GetHeightReal(xmaxw, zminw, false) + 4.0f, zminw),
		float3(xmaxw, CGround::GetHeightReal(xmaxw, zmaxw, false) + 4.0f, zmaxw),
		float3(xminw, CGround::GetHeightReal(xminw, zmaxw, false) + 4.0f, zmaxw),
		float3(xmidw, CGround::GetHeightReal(xmidw, zmidw, false) + 4.0f, zmidw),
	};
	static const unsigned char colors[3][4] = {
		{1 * 255, 0 * 255, 0 * 255, 1 * 255}, // red --> blocked
		{0 * 255, 1 * 255, 0 * 255, 1 * 255}, // green --> passable
		{0 * 255, 0 * 255, 1 *  64, 1 *  64}, // light blue --> pushed
	};

	const unsigned char* color =
		(!showCost)?
			&colors[2][0]:
		(node->AllSquaresImpassable())?
			&colors[0][0]:
			&colors[1][0];

	if (!batchDraw) {
		if (!camera->InView(verts[4]))
			return;

		va->Initialize();
		va->EnlargeArrays(4, 0, VA_SIZE_C);

		if (!fillQuad)
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}

	va->AddVertexQC(verts[0], color);
	va->AddVertexQC(verts[1], color);
	va->AddVertexQC(verts[2], color);
	va->AddVertexQC(verts[3], color);

	if (!batchDraw) {
		va->DrawArrayC(GL_QUADS);

		if (!fillQuad) {
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}
	}

	if (showCost && camera->GetPos().SqDistance(verts[4]) < (1000.0f * 1000.0f)) {
		font->SetTextColor(0.0f, 0.0f, 0.0f, 1.0f);
		font->glWorldPrint(verts[4], 5.0f, FloatToString(node->GetMoveCost(), "%8.2f"));
	}

	#undef xminw
	#undef xmaxw
	#undef zminw
	#undef zmaxw
	#undef xmidw
	#undef zmidw
}

void QTPFSPathDrawer::DrawNodeLink(const QTPFS::QTNode* pushedNode, const QTPFS::QTNode* poppedNode, CVertexArray* va) const {
	#define xmidw(n) (n->xmid() * SQUARE_SIZE)
	#define zmidw(n) (n->zmid() * SQUARE_SIZE)

	const float3 verts[2] = {
		float3(xmidw(pushedNode), CGround::GetHeightReal(xmidw(pushedNode), zmidw(pushedNode), false) + 4.0f, zmidw(pushedNode)),
		float3(xmidw(poppedNode), CGround::GetHeightReal(xmidw(poppedNode), zmidw(poppedNode), false) + 4.0f, zmidw(poppedNode)),
	};
	static const unsigned char color[4] = {
		1 * 255, 0 * 255, 1 * 255, 1 * 128,
	};

	if (!camera->InView(verts[0]) && !camera->InView(verts[1]))
		return;

	va->Initialize();
	va->EnlargeArrays(2, 0, VA_SIZE_C);
	va->AddVertexQC(verts[0], color);
	va->AddVertexQC(verts[1], color);
	glLineWidth(2);
	va->DrawArrayC(GL_LINES);
	glLineWidth(1);

	#undef xmidw
	#undef zmidw
}