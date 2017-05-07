#include "WavefrontPathTracer.h"

namespace CudaTracerLib {

enum
{
	MaxBlockHeight = 6,
};

CUDA_DEVICE int g_NextRayCounterWPT;
CUDA_DEVICE WavefrontPathTracerBuffer g_IntersectorWPT;
CUDA_DEVICE WavefrontPathTracerBuffer g_Intersector2WPT;
CUDA_DEVICE DeviceDepthImage g_DepthImageWPT;

__global__ void pathCreateKernelWPT(unsigned int w, unsigned int h)
{
	int rayidx;
	__shared__ volatile int nextRayArray[MaxBlockHeight];
	do
	{
		const int tidx = threadIdx.x;
		volatile int& rayBase = nextRayArray[threadIdx.y];

		const bool          terminated = 1;//nodeAddr == EntrypointSentinel;
		const unsigned int  maskTerminated = __ballot(terminated);
		const int           numTerminated = __popc(maskTerminated);
		const int           idxTerminated = __popc(maskTerminated & ((1u << tidx) - 1));

		if (terminated)
		{
			if (idxTerminated == 0)
				rayBase = atomicAdd(&g_NextRayCounterWPT, numTerminated);

			rayidx = rayBase + idxTerminated;
			if (rayidx >= w * h)
				break;
		}

		int x = rayidx % w, y = rayidx / w;
		NormalizedT<Ray> r;
		auto rng = g_SamplerData(rayidx);
		Spectrum W = g_SceneData.sampleSensorRay(r, Vec2f(x, y), rng.randomFloat2());
		traversalRay& ray = g_IntersectorWPT(rayidx, 0);
		ray.a = Vec4f(r.ori(), 0.0f);
		ray.b = Vec4f(r.dir(), FLT_MAX);
		auto& dat = g_IntersectorWPT(rayidx);
		dat.x = x;
		dat.y = y;
		dat.throughput = W;
		dat.L = Spectrum(0.0f);
		dat.dIdx = UINT_MAX;
		dat.specular_bounce = true;
	} while (true);
}

template<bool NEXT_EVENT_EST> __global__ void pathIterateKernel(unsigned int N, Image I, int pathDepth, int iterationIdx, int maxPathDepth, int RRStartDepth, bool depthImage)
{
	int rayidx;
	__shared__ volatile int nextRayArray[MaxBlockHeight];
	do
	{
		const int tidx = threadIdx.x;
		volatile int& rayBase = nextRayArray[threadIdx.y];

		const bool          terminated = 1;//nodeAddr == EntrypointSentinel;
		const unsigned int  maskTerminated = __ballot(terminated);
		const int           numTerminated = __popc(maskTerminated);
		const int           idxTerminated = __popc(maskTerminated & ((1u << tidx) - 1));

		if (terminated)
		{
			if (idxTerminated == 0)
				rayBase = atomicAdd(&g_NextRayCounterWPT, numTerminated);

			rayidx = rayBase + idxTerminated;
			if (rayidx >= N)
				break;
		}
		auto rng = g_SamplerData(rayidx);
		rng.skip(iterationIdx + 2);//plus the camera sample

		auto dat = g_IntersectorWPT(rayidx);
		if (NEXT_EVENT_EST && pathDepth > 0 && dat.dIdx != UINT_MAX)
		{
			traversalResult& res = g_IntersectorWPT.res(dat.dIdx, 1);
			traversalRay& ray = g_IntersectorWPT(dat.dIdx, 1);
			if (res.dist >= dat.dDist * (1 - 0.01f))
				dat.L += dat.directF;
			dat.dIdx = UINT_MAX;
			dat.directF = 0.0f;
		}

		traversalResult& res = g_IntersectorWPT.res(rayidx, 0);
		traversalRay& ray = g_IntersectorWPT(rayidx, 0);

		if (pathDepth == 0 && depthImage)
			g_DepthImageWPT.Store((int)dat.x.ToFloat(), (int)dat.y.ToFloat(), res.dist);

		if (res.dist)
		{
			NormalizedT<Ray> r(ray.a.getXYZ(), normalize(ray.b.getXYZ()));
			TraceResult r2;
			res.toResult(&r2, g_SceneData);
			BSDFSamplingRecord bRec;
			r2.getBsdfSample(r, bRec, ETransportMode::ERadiance);
			if (!NEXT_EVENT_EST || (pathDepth == 0 || dat.specular_bounce))
				dat.L += r2.Le(bRec.dg.P, bRec.dg.sys, -r.dir()) * dat.throughput;
			if (pathDepth + 1 != maxPathDepth)
			{
				Spectrum f = r2.getMat().bsdf.sample(bRec, rng.randomFloat2());
				dat.specular_bounce = (bRec.sampledType & EDelta) != 0;
				if (pathDepth >= RRStartDepth)
				{
					if (rng.randomFloat() < dat.throughput.max())
						dat.throughput /= dat.throughput.max();
					else goto labelAdd;
				}
				unsigned int idx2 = g_Intersector2WPT.insertRay(0);
				traversalRay& ray2 = g_Intersector2WPT(idx2, 0);
				ray2.a = Vec4f(bRec.dg.P, 1e-2f);
				ray2.b = Vec4f(bRec.getOutgoing(), FLT_MAX);

				dat.dIdx = UINT_MAX;
				if (NEXT_EVENT_EST)
				{
					DirectSamplingRecord dRec(bRec.dg.P, bRec.dg.sys.n);
					Spectrum value = g_SceneData.sampleEmitterDirect(dRec, rng.randomFloat2());
					if (r2.getMat().bsdf.hasComponent(ESmooth) && !value.isZero())
					{
						bRec.typeMask = EBSDFType(EAll & ~EDelta);
						bRec.wo = bRec.dg.toLocal(dRec.d);
						Spectrum bsdfVal = r2.getMat().bsdf.f(bRec);
						const float bsdfPdf = r2.getMat().bsdf.pdf(bRec);
						const float weight = MonteCarlo::PowerHeuristic(1, dRec.pdf, 1, bsdfPdf);
						dat.directF = dat.throughput * value * bsdfVal * weight;
						dat.dDist = dRec.dist;
						dat.dIdx = g_Intersector2WPT.insertRay(1);
						traversalRay& ray3 = g_Intersector2WPT(dat.dIdx, 1);
						ray3.a = Vec4f(bRec.dg.P, 1e-2f);
						ray3.b = Vec4f(dRec.d, FLT_MAX);
					}
				}
				dat.throughput *= f;
				g_Intersector2WPT(idx2) = dat;
			}
		}
		else dat.L += dat.throughput * g_SceneData.EvalEnvironment(Ray(ray.a.getXYZ(), ray.b.getXYZ()));

		if (!res.dist || pathDepth + 1 == maxPathDepth)
		{
		labelAdd:
			I.AddSample(dat.x.ToFloat(), dat.y.ToFloat(), dat.L);
		}
	} while (true);
}

void WavefrontPathTracer::DoRender(Image* I)
{
	if (hasDepthBuffer())
		CopyToSymbol(g_DepthImageWPT, getDeviceDepthBuffer());
	bufA->Clear();
	ZeroSymbol(g_NextRayCounterWPT);
	CopyToSymbol(g_IntersectorWPT, *bufA);
	pathCreateKernelWPT << < dim3(180, 1, 1), dim3(32, 6, 1) >> >(w, h);
	CopyFromSymbol(*bufA, g_IntersectorWPT);
	bufA->setNumRays(w * h, 0);
	int pass = 0, maxPathLength = m_sParameters.getValue(KEY_MaxPathLength()), rrStart = m_sParameters.getValue(KEY_RRStartDepth());
	WavefrontPathTracerBuffer* srcBuf = bufA, *destBuf = bufB;
	do
	{
		destBuf->Clear();
		srcBuf->IntersectBuffers<false>(false);
		CopyToSymbol(g_IntersectorWPT, *srcBuf); CopyToSymbol(g_Intersector2WPT, *destBuf);
		ZeroSymbol(g_NextRayCounterWPT);
		if (m_sParameters.getValue(KEY_Direct()))
			pathIterateKernel<true> << < dim3(180, 1, 1), dim3(32, 6, 1) >> >(srcBuf->getNumRays(0), *I, pass, m_uPassesDone, maxPathLength, rrStart, hasDepthBuffer());
		else pathIterateKernel<false> << < dim3(180, 1, 1), dim3(32, 6, 1) >> >(srcBuf->getNumRays(0), *I, pass, m_uPassesDone, maxPathLength, rrStart, hasDepthBuffer());
		CopyFromSymbol(*srcBuf, g_IntersectorWPT); CopyFromSymbol(*destBuf, g_Intersector2WPT);
		swapk(srcBuf, destBuf);
	} while (srcBuf->getNumRays(0) && ++pass < maxPathLength);
}

}