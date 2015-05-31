#include <StdAfx.h>
#include "SplitBVHBuilder.hpp"

template<typename T> void removeSwap(std::vector<T>& vec, int i)
{
	T val = vec[vec.size() - 1];
	vec[i] = val;
	vec.pop_back();
}

template<typename T> T removeLast(std::vector<T>& vec)
{
	T val = vec[vec.size() - 1];
	vec.pop_back();
	return val;
}

#define QSORT_STACK_SIZE    32
#define QSORT_MIN_SIZE      16
typedef bool(*SortCompareFunc) (void* data, int idxA, int idxB);    // Returns true if A should come before B.
typedef void(*SortSwapFunc)    (void* data, int idxA, int idxB);    // Swaps A and B.

void insertionSort(int start, int size, void* data, SortCompareFunc compareFunc, SortSwapFunc swapFunc)
{
	CT_ASSERT(compareFunc && swapFunc);
	CT_ASSERT(size >= 0);

	for (int i = 1; i < size; i++)
	{
		int j = start + i - 1;
		while (j >= start && compareFunc(data, j + 1, j))
		{
			swapFunc(data, j, j + 1);
			j--;
		}
	}
}

int median3(int low, int high, void* data, SortCompareFunc compareFunc)
{
	CT_ASSERT(compareFunc);
	CT_ASSERT(low >= 0 && high >= 2);

	int l = low;
	int c = (low + high) >> 1;
	int h = high - 2;

	if (compareFunc(data, h, l)) swapk(l, h);
	if (compareFunc(data, c, l)) c = l;
	return (compareFunc(data, h, c)) ? h : c;
}

//------------------------------------------------------------------------

int partition(int low, int high, void* data, SortCompareFunc compareFunc, SortSwapFunc swapFunc)
{
	// Select pivot using median-3, and hide it in the highest entry.

	swapFunc(data, median3(low, high, data, compareFunc), high - 1);

	// Partition data.

	int i = low - 1;
	int j = high - 1;
	for (;;)
	{
		do
			i++;
		while (compareFunc(data, i, high - 1));
		do
			j--;
		while (compareFunc(data, high - 1, j));

		CT_ASSERT(i >= low && j >= low && i < high && j < high);
		if (i >= j)
			break;

		swapFunc(data, i, j);
	}

	// Restore pivot.

	swapFunc(data, i, high - 1);
	return i;
}

//------------------------------------------------------------------------

void qsort(int low, int high, void* data, SortCompareFunc compareFunc, SortSwapFunc swapFunc)
{
	CT_ASSERT(compareFunc && swapFunc);
	CT_ASSERT(low <= high);

	int stack[QSORT_STACK_SIZE];
	int sp = 0;
	stack[sp++] = high;

	while (sp)
	{
		high = stack[--sp];
		CT_ASSERT(low <= high);

		// Small enough or stack full => use insertion sort.

		if (high - low < QSORT_MIN_SIZE || sp + 2 > QSORT_STACK_SIZE)
		{
			insertionSort(low, high - low, data, compareFunc, swapFunc);
			low = high + 1;
			continue;
		}

		// Partition and sort sub-partitions.

		int i = partition(low, high, data, compareFunc, swapFunc);
		CT_ASSERT(sp + 2 <= QSORT_STACK_SIZE);
		if (high - i > 2)
			stack[sp++] = high;
		if (i - low > 1)
			stack[sp++] = i;
		else
			low = i + 1;
	}
}

void sort(void* data, int start, int end, SortCompareFunc compareFunc, SortSwapFunc swapFunc)
{
	CT_ASSERT(start <= end);
	CT_ASSERT(compareFunc && swapFunc);

	// Nothing to do => skip.

	if (end - start < 2)
		return;

	qsort(start, end, data, compareFunc, swapFunc);
}

//------------------------------------------------------------------------

SplitBVHBuilder::SplitBVHBuilder(IBVHBuilderCallback* clb, const Platform& P, const BuildParams& stats)
	: m_pClb(clb),
    m_platform      (P),
	m_params(stats),
    m_minOverlap    (0.0f),
    m_sortDim       (-1)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < NumSpatialBins; j++)
			m_bins[i][j].bounds = AABB::Identity();
}

//------------------------------------------------------------------------

SplitBVHBuilder::~SplitBVHBuilder(void)
{
}

//------------------------------------------------------------------------

int handleNode(BVHNode* n, IBVHBuilderCallback* clb, std::vector<int>& m_Indices, int level = 0)
{
	if (n->isLeaf())
	{
		LeafNode* leaf = (LeafNode*)n;
		if (leaf->getNumTriangles() == 0)
			return 0x76543210;
		if (level)
		{
			int idx = clb->handleLeafObjects(m_Indices[leaf->m_lo]);
			for (int j = leaf->m_lo + 1; j < leaf->m_hi; j++)
				clb->handleLeafObjects(m_Indices[j]);
			clb->handleLastLeafObject();
			return ~idx;
		}
		else
		{
			int idx;
			e_BVHNodeData* node = clb->HandleNodeAllocation(&idx);
			int idx2 = clb->handleLeafObjects(m_Indices[leaf->m_lo]);
			for (int j = leaf->m_lo + 1; j < leaf->m_hi; j++)
				clb->handleLeafObjects(m_Indices[j]);
			clb->handleLastLeafObject();

			node->setChildren(Vec2i(~idx2, 0x76543210));
			node->setLeft(n->m_bounds);
			node->setRight(AABB(Vec3f(0.0f), Vec3f(0.0f)));
			return idx;
		}

	}
	else
	{
		int idx;
		e_BVHNodeData* node = clb->HandleNodeAllocation(&idx);
		InnerNode* n2 = (InnerNode*)n;
		int a = handleNode(n2->getChildNode(0), clb, m_Indices, level + 1);
		int b = handleNode(n2->getChildNode(1), clb, m_Indices, level + 1);
		node->setChildren(Vec2i(a, b));
		node->setLeft(n2->getChildNode(0)->m_bounds);
		node->setRight(n2->getChildNode(1)->m_bounds);
		return idx;
	}
}

void SplitBVHBuilder::run(void)
{
    // Initialize reference stack and determine root bounds.

    NodeSpec rootSpec;
    rootSpec.numRef = m_pClb->Count();
	rootSpec.bounds = AABB::Identity();
    m_refStack.resize(rootSpec.numRef);

    for (int i = 0; i < rootSpec.numRef; i++)
    {
        m_refStack[i].triIdx = i;
		m_pClb->getBox(i, &m_refStack[i].bounds);
        rootSpec.bounds.Enlarge(m_refStack[i].bounds);
    }

    // Initialize rest of the members.

    m_minOverlap = rootSpec.bounds.Area() * m_params.splitAlpha;
	m_rightBounds.resize(max(rootSpec.numRef, (int)NumSpatialBins));
    m_numDuplicates = 0;
	m_Timer.StartTimer();

    // Build recursively.

    BVHNode* root = buildNode(rootSpec, 0, 0.0f, 1.0f);

    // Done.
	*(bool*)&m_params.enablePrints = false;
        printf("SplitBVHBuilder: progress %.0f%%, duplicates %.0f%%\n",
		100.0f, (float)m_numDuplicates / (float)m_pClb->Count() * 100.0f);

	m_pClb->HandleBoundingBox(rootSpec.bounds);
	m_pClb->HandleStartNode(handleNode(root, m_pClb, m_Indices));

    delete root;
}

//------------------------------------------------------------------------

bool SplitBVHBuilder::sortCompare(void* data, int idxA, int idxB)
{
    const SplitBVHBuilder* ptr = (const SplitBVHBuilder*)data;
    int dim = ptr->m_sortDim;
    const Reference& ra = ptr->m_refStack[idxA];
    const Reference& rb = ptr->m_refStack[idxB];
    float ca = ra.bounds.minV[dim] + ra.bounds.maxV[dim];
	float cb = rb.bounds.minV[dim] + rb.bounds.maxV[dim];
    return (ca < cb || (ca == cb && ra.triIdx < rb.triIdx));
}

//------------------------------------------------------------------------

void SplitBVHBuilder::sortSwap(void* data, int idxA, int idxB)
{
    SplitBVHBuilder* ptr = (SplitBVHBuilder*)data;
    swapk(ptr->m_refStack[idxA], ptr->m_refStack[idxB]);
}

//------------------------------------------------------------------------

BVHNode* SplitBVHBuilder::buildNode(NodeSpec spec, int level, float progressStart, float progressEnd)
{
    // Display progress.

    if ( m_Timer.EndTimer() >= 1.0f)
    {
        printf("SplitBVHBuilder: progress %.0f%%, duplicates %.0f%%\r",
			progressStart * 100.0f, (float)m_numDuplicates / (float)m_pClb->Count() * 100.0f);
		m_Timer.StartTimer();
    }

    // Remove degenerates.
    {
		int firstRef = (int)m_refStack.size() - spec.numRef;
		for (int i = (int)m_refStack.size() - 1; i >= firstRef; i--)
        {
            Vec3f size = m_refStack[i].bounds.maxV - m_refStack[i].bounds.minV;
            if (min(size) < 0.0f || sum(size) == max(size))
				removeSwap(m_refStack, i);
        }
		spec.numRef = (int)m_refStack.size() - firstRef;
    }

    // Small enough or too deep => create leaf.

    if (spec.numRef <= m_platform.getMinLeafSize() || level >= MaxDepth)
        return createLeaf(spec);

    // Find split candidates.

    float area = spec.bounds.Area();
    float leafSAH = area * m_platform.getTriangleCost(spec.numRef);
    float nodeSAH = area * m_platform.getNodeCost(2);
    ObjectSplit object = findObjectSplit(spec, nodeSAH);

    SpatialSplit spatial;
    if (level < MaxSpatialDepth)
    {
        AABB overlap = object.leftBounds;
        overlap.intersect(object.rightBounds);
        if (overlap.Area() >= m_minOverlap)
            spatial = findSpatialSplit(spec, nodeSAH);
    }

    // Leaf SAH is the lowest => create leaf.

    float minSAH = min(leafSAH, object.sah, spatial.sah);
    if (minSAH == leafSAH && spec.numRef <= m_platform.getMaxLeafSize())
        return createLeaf(spec);

    // Perform split.

    NodeSpec left, right;
    if (minSAH == spatial.sah)
        performSpatialSplit(left, right, spec, spatial);
    if (!left.numRef || !right.numRef)
        performObjectSplit(left, right, spec, object);

    // Create inner node.

    m_numDuplicates += left.numRef + right.numRef - spec.numRef;
    float progressMid = math::lerp(progressStart, progressEnd, (float)right.numRef / (float)(left.numRef + right.numRef));
    BVHNode* rightNode = buildNode(right, level + 1, progressStart, progressMid);
    BVHNode* leftNode = buildNode(left, level + 1, progressMid, progressEnd);
    return new InnerNode(spec.bounds, leftNode, rightNode);
}

//------------------------------------------------------------------------

BVHNode* SplitBVHBuilder::createLeaf(const NodeSpec& spec)
{
    for (int i = 0; i < spec.numRef; i++)
		m_Indices.push_back(removeLast(m_refStack).triIdx);
	return new LeafNode(spec.bounds, (int)m_Indices.size() - spec.numRef, (int)m_Indices.size());
}

//------------------------------------------------------------------------

SplitBVHBuilder::ObjectSplit SplitBVHBuilder::findObjectSplit(const NodeSpec& spec, float nodeSAH)
{
    ObjectSplit split;
    const Reference* refPtr = &m_refStack[m_refStack.size() - spec.numRef];
	float bestTieBreak = FLT_MAX;

    // Sort along each dimension.

    for (m_sortDim = 0; m_sortDim < 3; m_sortDim++)
    {
		sort(this, (int)m_refStack.size() - spec.numRef, (int)m_refStack.size(), sortCompare, sortSwap);

        // Sweep right to left and determine bounds.

        AABB rightBounds = AABB::Identity();
        for (int i = spec.numRef - 1; i > 0; i--)
        {
            rightBounds.Enlarge(refPtr[i].bounds);
            m_rightBounds[i - 1] = rightBounds;
        }

        // Sweep left to right and select lowest SAH.

		AABB leftBounds = AABB::Identity();
        for (int i = 1; i < spec.numRef; i++)
        {
            leftBounds.Enlarge(refPtr[i - 1].bounds);
			float lA = leftBounds.Area(), rA = m_rightBounds[i - 1].Area();
            float sah = nodeSAH + lA * m_platform.getTriangleCost(i) + rA * m_platform.getTriangleCost(spec.numRef - i);
			float tieBreak = math::sqr((float)i) + math::sqr((float)(spec.numRef - i));
            if (sah < split.sah || (sah == split.sah && tieBreak < bestTieBreak))
            {
                split.sah = sah;
                split.sortDim = m_sortDim;
                split.numLeft = i;
                split.leftBounds = leftBounds;
                split.rightBounds = m_rightBounds[i - 1];
                bestTieBreak = tieBreak;
            }
        }
    }
    return split;
}

//------------------------------------------------------------------------

void SplitBVHBuilder::performObjectSplit(NodeSpec& left, NodeSpec& right, const NodeSpec& spec, const ObjectSplit& split)
{
    m_sortDim = split.sortDim;
	sort(this, (int)m_refStack.size() - spec.numRef, (int)m_refStack.size(), sortCompare, sortSwap);

    left.numRef = split.numLeft;
    left.bounds = split.leftBounds;
    right.numRef = spec.numRef - split.numLeft;
    right.bounds = split.rightBounds;
}

//------------------------------------------------------------------------

SplitBVHBuilder::SpatialSplit SplitBVHBuilder::findSpatialSplit(const NodeSpec& spec, float nodeSAH)
{
    // Initialize bins.

    Vec3f origin = spec.bounds.minV;
	Vec3f binSize = (spec.bounds.maxV - origin) * (1.0f / (float)NumSpatialBins);
    Vec3f invBinSize = 1.0f / binSize;

    for (int dim = 0; dim < 3; dim++)
    {
        for (int i = 0; i < NumSpatialBins; i++)
        {
            SpatialBin& bin = m_bins[dim][i];
            bin.bounds = AABB::Identity();
            bin.enter = 0;
            bin.exit = 0;
        }
    }

    // Chop references into bins.

    for (int refIdx = (int)m_refStack.size() - spec.numRef; refIdx < m_refStack.size(); refIdx++)
    {
        const Reference& ref = m_refStack[refIdx];
        Vec3i firstBin = clamp(Vec3i((ref.bounds.minV - origin) * invBinSize), Vec3i(0), Vec3i(NumSpatialBins - 1));
        Vec3i lastBin = clamp(Vec3i((ref.bounds.maxV - origin) * invBinSize), Vec3i(firstBin), Vec3i(NumSpatialBins - 1));

        for (int dim = 0; dim < 3; dim++)
        {
            Reference currRef = ref;
            for (int i = firstBin[dim]; i < lastBin[dim]; i++)
            {
                Reference leftRef, rightRef;
				splitReference(leftRef, rightRef, currRef, dim, origin[dim] + binSize[dim] * (float)(i + 1));
                m_bins[dim][i].bounds.Enlarge(leftRef.bounds);
                currRef = rightRef;
            }
            m_bins[dim][lastBin[dim]].bounds.Enlarge(currRef.bounds);
            m_bins[dim][firstBin[dim]].enter++;
            m_bins[dim][lastBin[dim]].exit++;
        }
    }

    // Select best split plane.

    SpatialSplit split;
    for (int dim = 0; dim < 3; dim++)
    {
        // Sweep right to left and determine bounds.

		AABB rightBounds = AABB::Identity();
        for (int i = NumSpatialBins - 1; i > 0; i--)
        {
            rightBounds.Enlarge(m_bins[dim][i].bounds);
            m_rightBounds[i - 1] = rightBounds;
        }

        // Sweep left to right and select lowest SAH.

		AABB leftBounds = AABB::Identity();
        int leftNum = 0;
        int rightNum = spec.numRef;

        for (int i = 1; i < NumSpatialBins; i++)
        {
            leftBounds.Enlarge(m_bins[dim][i - 1].bounds);
            leftNum += m_bins[dim][i - 1].enter;
            rightNum -= m_bins[dim][i - 1].exit;

			float sah = nodeSAH + leftBounds.Area() * m_platform.getTriangleCost(leftNum) + m_rightBounds[i - 1].Area() * m_platform.getTriangleCost(rightNum);
            if (sah < split.sah)
            {
                split.sah = sah;
                split.dim = dim;
				split.pos = origin[dim] + binSize[dim] * (float)i;
            }
        }
    }
    return split;
}

//------------------------------------------------------------------------

void SplitBVHBuilder::performSpatialSplit(NodeSpec& left, NodeSpec& right, const NodeSpec& spec, const SpatialSplit& split)
{
    // Categorize references and compute bounds.
    //
    // Left-hand side:      [leftStart, leftEnd[
    // Uncategorized/split: [leftEnd, rightStart[
    // Right-hand side:     [rightStart, refs.getSize()[

    std::vector<Reference>& refs = m_refStack;
    int leftStart = (int)refs.size() - spec.numRef;
    int leftEnd = leftStart;
    int rightStart = (int)refs.size();
    left.bounds = right.bounds = AABB::Identity();

    for (int i = leftEnd; i < rightStart; i++)
    {
        // Entirely on the left-hand side?

        if (refs[i].bounds.maxV[split.dim] <= split.pos)
        {
            left.bounds.Enlarge(refs[i].bounds);
            swapk(refs[i], refs[leftEnd++]);
        }

        // Entirely on the right-hand side?

        else if (refs[i].bounds.minV[split.dim] >= split.pos)
        {
            right.bounds.Enlarge(refs[i].bounds);
            swapk(refs[i--], refs[--rightStart]);
        }
    }

    // Duplicate or unsplit references intersecting both sides.

    while (leftEnd < rightStart)
    {
        // Split reference.

        Reference lref, rref;
        splitReference(lref, rref, refs[leftEnd], split.dim, split.pos);

        // Compute SAH for duplicate/unsplit candidates.

        AABB lub = left.bounds;  // Unsplit to left:     new left-hand bounds.
        AABB rub = right.bounds; // Unsplit to right:    new right-hand bounds.
        AABB ldb = left.bounds;  // Duplicate:           new left-hand bounds.
        AABB rdb = right.bounds; // Duplicate:           new right-hand bounds.
        lub.Enlarge(refs[leftEnd].bounds);
        rub.Enlarge(refs[leftEnd].bounds);
        ldb.Enlarge(lref.bounds);
        rdb.Enlarge(rref.bounds);

        float lac = m_platform.getTriangleCost(leftEnd - leftStart);
		float rac = m_platform.getTriangleCost((int)refs.size() - rightStart);
        float lbc = m_platform.getTriangleCost(leftEnd - leftStart + 1);
        float rbc = m_platform.getTriangleCost((int)refs.size() - rightStart + 1);

        float unsplitLeftSAH = lub.Area() * lbc + right.bounds.Area() * rac;
        float unsplitRightSAH = left.bounds.Area() * lac + rub.Area() * rbc;
        float duplicateSAH = ldb.Area() * lbc + rdb.Area() * rbc;
        float minSAH = min(unsplitLeftSAH, unsplitRightSAH, duplicateSAH);

        // Unsplit to left?

        if (minSAH == unsplitLeftSAH)
        {
            left.bounds = lub;
            leftEnd++;
        }

        // Unsplit to right?

        else if (minSAH == unsplitRightSAH)
        {
            right.bounds = rub;
            swapk(refs[leftEnd], refs[--rightStart]);
        }

        // Duplicate?

        else
        {
            left.bounds = ldb;
            right.bounds = rdb;
            refs[leftEnd++] = lref;
            refs.push_back(rref);
        }
    }

    left.numRef = leftEnd - leftStart;
	right.numRef = (int)refs.size() - rightStart;
}

//------------------------------------------------------------------------

void SplitBVHBuilder::splitReference(Reference& left, Reference& right, const Reference& ref, int dim, float pos)
{
	m_pClb->SplitNode(ref.triIdx, dim, pos, left.bounds, right.bounds, ref.bounds);
	left.triIdx = right.triIdx = ref.triIdx;
}

//------------------------------------------------------------------------
