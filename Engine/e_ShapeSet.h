#pragma once

#include "..\Math\vector.h"
#include "..\Math\Montecarlo.h"
#include "..\Engine\e_Mesh.h"

template<int N> struct ShapeSet
{
	struct triData
	{
		CUDA_ALIGN(16) e_TriIntersectorData dat;
		CUDA_ALIGN(16) float3 p[3];
		CUDA_ALIGN(16) float3 n;
		float area;
		unsigned int index;
		
		triData(){}
		triData(const e_TriIntersectorData* a_Int, const e_TriIntersectorData* start, unsigned int i)
		{
			dat = *a_Int;
			index = i;
			a_Int->getData(p[0], p[1], p[2]);
			n = -cross(p[2] - p[0], p[1] - p[0]);
			area = length(n);
			n *= 1.0f / area;
		}
		CUDA_FUNC_IN float3 rndPoint(float b1, float b2) const
		{
			return b1 * p[0] + b2 * p[1] + (1.f - b1 - b2) * p[2];
		}
		CUDA_FUNC_IN float3 Sample(float u1, float u2, float3* Ns) const
		{
			float b1, b2;
			UniformSampleTriangle(u1, u2, &b1, &b2);
			*Ns = nor();
			return rndPoint(b1, b2);
		}
		CUDA_FUNC_IN float3 nor() const
		{
			return n;
		}
		AABB box() const
		{
			AABB b = AABB::Identity();
			for(int i = 0; i < 3; i++)
				b.Enlarge(p[i]);
			return b;
		}
	};
public:
    ShapeSet(e_TriIntersectorData** indices, e_TriIntersectorData* A, unsigned int* indices2, unsigned int indexCount)
	{
		if(indexCount > N)
			throw 1;
		count = indexCount;
		float areas[N];
		sumArea = 0;
		for(int i = 0; i < count; i++)
		{
			tris[i] = triData(indices[i], A, indices2[i]);
			areas[i] = tris[i].area;
			sumArea += tris[i].area;
		}
		areaDistribution = Distribution1D<N>(areas, count);
	}
    CUDA_FUNC_IN float Area() const { return sumArea; }
    CUDA_FUNC_IN float3 Sample(const LightSample &ls, float3 *Ns, const e_TriIntersectorData* a_Int) const
	{
		const triData& sn = tris[areaDistribution.SampleDiscrete(ls.uComponent, NULL)];
		return sn.Sample(ls.uPos[0], ls.uPos[1], Ns);
	}
    CUDA_FUNC_IN float3 Sample(const float3 &p, const LightSample &ls, float3 *Ns, const e_TriIntersectorData* a_Int) const
	{
		int sn = areaDistribution.SampleDiscrete(ls.uComponent, NULL);
		float3 pt = tris[sn].Sample(ls.uPos[0], ls.uPos[1], Ns);
		TraceResult r2;
		r2.Init();
		float3 n;
		Ray r(p, normalize(pt - p));
		for (int i = 0; i < count; ++i)
			if(tris[i].dat.Intersect(r, &r2))//a_Int[tris[i].index]
				n = tris[i].n;
		if(r2.hasHit() && Ns)
			*Ns = n;
		return r(r2.m_fDist);
		return pt;
	}
    CUDA_FUNC_IN float Pdf(const float3 &p, const float3 &wi, const e_TriIntersectorData* a_Int) const
	{
		float pdf = 0.f;
		TraceResult r;
		for (int i = 0; i < count; i++)
		{
			r.Init();
			if(tris[i].dat.Intersect(Ray(p, wi), &r))
			{
				float pdf2 = r.m_fDist * length(wi) / (AbsDot(tris[i].nor(), -1.0f * wi) * tris[i].area);
				if (pdf2 == pdf2) 
					pdf += tris[i].area * pdf2;
			}
		}
		return pdf / sumArea;
	}
    CUDA_FUNC_IN float Pdf(const float3 &p) const
	{
		return float(count) / sumArea;
	}
	CUDA_FUNC_IN bool Contains(unsigned int index) const
	{
		for(int i = 0; i < count; i++)
			if(index == tris[i].index)
				return true;
		return false;
	}
	AABB getBox() const
	{
		AABB b = AABB::Identity();
		for(int i = 0; i < count; i++)
			b.Enlarge(tris[i].box());
		return b;
	}
private:
    triData tris[N];
    float sumArea;
    Distribution1D<N> areaDistribution;
	int count;
};