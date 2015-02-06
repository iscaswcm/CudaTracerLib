#pragma once

#include "..\MathTypes.h"
#include "..\Kernel\k_TraceAlgorithms.h"

CUDA_FUNC_IN float Mis(float pdf)
{
	return pdf;
}

CUDA_FUNC_IN float PdfWtoA(const float aPdfW, const float aDist2, const float aCosThere)
{
	return aPdfW * std::abs(aCosThere) / aDist2;
}

CUDA_FUNC_IN float PdfAtoW(const float aPdfA, const float aDist2, const float aCosThere)
{
	return aPdfA * aDist2 / std::abs(aCosThere);
}

CUDA_FUNC_IN float pdf(const e_KernelMaterial& mat, BSDFSamplingRecord& bRec)
{
	bRec.typeMask = EAll;
	if (mat.bsdf.hasComponent(EDelta))
		return mat.bsdf.pdf(bRec, EDiscrete);
	return mat.bsdf.pdf(bRec) / fabsf(Frame::cosTheta(bRec.wo));
}

CUDA_FUNC_IN float revPdf(const e_KernelMaterial& mat, BSDFSamplingRecord& bRec)
{
	bRec.typeMask = EAll;
	if (mat.bsdf.hasComponent(EDelta))
		return mat.bsdf.pdf(bRec, EDiscrete);
	swapk(bRec.wo, bRec.wi);
	float pdf = mat.bsdf.pdf(bRec) / fabsf(Frame::cosTheta(bRec.wo));
	swapk(bRec.wo, bRec.wi);
	return pdf;
}

struct BPTSubPathState
{
	Ray r;
	Spectrum throughput;
	bool delta;

	float dVCM;
	float dVC;
	float dVM;
};

struct BPTVertex
{
	const e_KernelMaterial* mat;
	DifferentialGeometry dg;
	BSDFSamplingRecord bRec;
	Spectrum throughput;
	int subPathLength;

	float dVCM;
	float dVC;
	float dVM;

	CUDA_FUNC_IN BPTVertex()
		: bRec(dg)
	{

	}
};

CUDA_FUNC_IN void sampleEmitter(BPTSubPathState& v, CudaRNG& rng, float mMisVcWeightFactor)
{
	PositionSamplingRecord pRec;
	DirectionSamplingRecord dRec;
	Spectrum Le = g_SceneData.sampleEmitterPosition(pRec, rng.randomFloat2());
	const e_KernelLight* l = (const e_KernelLight*)pRec.object;
	Le *= l->sampleDirection(dRec, pRec, rng.randomFloat2());
	float emitterPdf = g_SceneData.pdfLight(l);

	v.delta = 0;
	v.throughput = Le;
	v.r = Ray(pRec.p, dRec.d);

	DirectSamplingRecord directRec;
	directRec.d = directRec.refN = Vec3f(1, 0, 0);
	directRec.n = Vec3f(-1, 0, 0);
	directRec.measure = EArea;

	float directPdfW = l->pdfDirect(directRec) * emitterPdf;
	float emissionPdfW = pRec.pdf * dRec.pdf;

	v.dVCM = Mis(directPdfW / emissionPdfW);
	if (!l->As()->IsDegenerate())
	{
		float usedCosLight = dot(pRec.n, dRec.d);
		v.dVC = Mis(usedCosLight / emissionPdfW);
	}
	else v.dVC = 0;
	v.dVM = v.dVC * mMisVcWeightFactor;
}

CUDA_FUNC_IN void sampleCamera(BPTSubPathState& v, CudaRNG& rng, const Vec2f& pixelPosition, float mLightSubPathCount)
{
	PositionSamplingRecord pRec;
	DirectionSamplingRecord dRec;

	Spectrum imp = g_SceneData.m_Camera.samplePosition(pRec, rng.randomFloat2(), &pixelPosition);
	imp *= g_SceneData.m_Camera.sampleDirection(dRec, pRec, rng.randomFloat2(), &pixelPosition);

	float cameraPdfW = pRec.pdf * dRec.pdf;

	v.r = Ray(pRec.p, dRec.d);
	v.throughput = imp;
	v.delta = 1;
	v.dVCM = Mis(mLightSubPathCount / cameraPdfW);
	v.dVC = 0;
	v.dVM = 0;

}

CUDA_FUNC_IN bool sampleScattering(BPTSubPathState& v, BSDFSamplingRecord& bRec, const e_KernelMaterial& mat, CudaRNG& rng, float mMisVcWeightFactor, float mMisVmWeightFactor)
{
	float bsdfDirPdfW;
	Spectrum f = mat.bsdf.sample(bRec, bsdfDirPdfW, rng.randomFloat2());
	float cosThetaOut = fabsf(Frame::cosTheta(bRec.wo));
	float bsdfRevPdfW = bsdfDirPdfW;
	if ((bRec.sampledType & EDelta) == 0)
	{
		bsdfDirPdfW /= cosThetaOut;
		bsdfRevPdfW = revPdf(mat, bRec);
	}

	if (bRec.sampledType & EDelta)
	{
		v.dVCM = 0.f;
		v.dVC *= Mis(cosThetaOut);
		v.dVM *= Mis(cosThetaOut);
		v.delta &= 1;
	}
	else
	{
		v.dVC = Mis(cosThetaOut / bsdfDirPdfW) * (
			v.dVC * Mis(bsdfRevPdfW) +
			v.dVCM + mMisVmWeightFactor);

		v.dVM = Mis(cosThetaOut / bsdfDirPdfW) * (
			v.dVM * Mis(bsdfRevPdfW) +
			v.dVCM * mMisVcWeightFactor + 1.f);

		v.dVCM = Mis(1.f / bsdfDirPdfW);

		v.delta &= 0;
	}

	v.r = Ray(bRec.dg.P, bRec.getOutgoing());
	v.throughput *= f;
	return true;
}

 CUDA_FUNC_IN Spectrum gatherLight(const BPTSubPathState& cameraState, BSDFSamplingRecord& bRec, const TraceResult& r2, CudaRNG& rng, int subPathLength, bool use_mis)
{
	e_KernelLight* l = &g_SceneData.m_sLightData[r2.LightIndex()];
	float pdfLight = g_SceneData.pdfLight(l);
	PositionSamplingRecord pRec(bRec.dg.P, bRec.dg.sys.n, 0);
	float directPdfA = l->pdfPosition(pRec);
	DirectionSamplingRecord dRec(-cameraState.r.direction);
	float emissionPdfW = l->pdfDirection(dRec, pRec) * directPdfA;
	Spectrum L = l->eval(bRec.dg.P, bRec.dg.sys, -cameraState.r.direction);

	if (L.isZero())
		return Spectrum(0.0f);

	if (subPathLength == 1)
		return L;

	directPdfA *= pdfLight;
	emissionPdfW *= pdfLight;

	const float wCamera = Mis(directPdfA) * cameraState.dVCM +
		Mis(emissionPdfW) * cameraState.dVC;

	const float misWeight = use_mis ? 1.f / (1.f + wCamera) : 1;

	return misWeight * L;
}

 CUDA_FUNC_IN void connectToCamera(const BPTSubPathState& lightState, BSDFSamplingRecord& bRec, const e_KernelMaterial& mat, e_Image& g_Image, CudaRNG& rng, float mLightSubPathCount, float mMisVmWeightFactor, float scaleLight, bool use_mis)
{
	DirectSamplingRecord dRec(bRec.dg.P, bRec.dg.sys.n);
	Spectrum directFactor = g_SceneData.m_Camera.sampleDirect(dRec, rng.randomFloat2());
	bRec.wo = bRec.dg.toLocal(dRec.d);
	Spectrum bsdfFactor = mat.bsdf.f(bRec);
	float bsdfRevPdfW = revPdf(mat, bRec);//AAAA
	float cosToCamera = fabsf(Frame::cosTheta(bRec.wo));
	float cameraPdfA = dRec.pdf;
	if (dRec.measure != EArea)
		cameraPdfA = PdfWtoA(cameraPdfA, distanceSquared(bRec.dg.P, dRec.p), dot(dRec.n, -dRec.d));
	const float wLight = Mis(cameraPdfA / mLightSubPathCount) * (
						 mMisVmWeightFactor + lightState.dVCM + lightState.dVC * Mis(bsdfRevPdfW));
	float miWeight = use_mis ? 1.f / (wLight + 1.f) : 1;
	Spectrum contrib = miWeight * lightState.throughput * bsdfFactor * directFactor / mLightSubPathCount;
	if (!contrib.isZero() && ::V(dRec.p, dRec.ref))
		g_Image.Splat(dRec.uv.x, dRec.uv.y, contrib * scaleLight);
}

 CUDA_FUNC_IN Spectrum connectToLight(const BPTSubPathState& cameraState, BSDFSamplingRecord& bRec, const e_KernelMaterial& mat, CudaRNG& rng, float mMisVmWeightFactor, bool use_mis)
{
	DirectSamplingRecord dRec(bRec.dg.P, bRec.dg.sys.n);
	const Spectrum directFactor = g_SceneData.sampleEmitterDirect(dRec, rng.randomFloat2());
	const e_KernelLight* l = (const e_KernelLight*)dRec.object;
	if (!l)
		return Spectrum(0.0f);
	float pdfLight = g_SceneData.pdfLight(l);
	float directPdfW = dRec.pdf;
	DirectionSamplingRecord dirRec(-dRec.d);
	const float emissionPdfW = l->pdfPosition(dRec) * l->pdfDirection(dirRec, dRec) * pdfLight;
	const float cosAtLight = dot(dRec.n, -dRec.d);

	bRec.wo = bRec.dg.toLocal(dRec.d);
	const float cosToLight = fabsf(Frame::cosTheta(bRec.wo));
	const Spectrum bsdfFactor = mat.bsdf.f(bRec);
	const float bsdfDirPdfW = pdf(mat, bRec);
	const float bsdfRevPdfW = revPdf(mat, bRec);

	if (directFactor.isZero() || bsdfFactor.isZero())
		return Spectrum(0.0f);

	if (dRec.measure != ESolidAngle)
		directPdfW = PdfAtoW(directPdfW, distanceSquared(bRec.dg.P, dRec.p), cosAtLight);

	const float wLight = Mis(bsdfDirPdfW / directPdfW);
	const float wCamera = Mis(emissionPdfW * cosToLight / (directPdfW * cosAtLight)) * (
		mMisVmWeightFactor + cameraState.dVCM + cameraState.dVC * Mis(bsdfRevPdfW));

	const float misWeight = use_mis ? 1.f / (wLight + 1.f + wCamera) : 1;

	Spectrum contrib = misWeight * directFactor * bsdfFactor;

	if (contrib.isZero() || !::V(bRec.dg.P, dRec.p))
		return Spectrum(0.0f);
	return contrib;
}

 CUDA_FUNC_IN Spectrum connectVertices(BPTVertex& emitterVertex, const BPTSubPathState& cameraState, BSDFSamplingRecord& bRec, const e_KernelMaterial& mat, float mMisVcWeightFactor, float mMisVmWeightFactor, bool use_mis)
{
	const float dist2 = distanceSquared(emitterVertex.dg.P, bRec.dg.P);
	Vec3f direction = normalize(emitterVertex.dg.P - bRec.dg.P);

	bRec.wo = bRec.dg.toLocal(direction);
	const Spectrum cameraBsdf = mat.bsdf.f(bRec);
	const float cosCamera = fabsf(Frame::cosTheta(bRec.wo));
	const float cameraBsdfDirPdfW = pdf(mat, bRec);
	const float cameraBsdfRevPdfW = revPdf(mat, bRec);

	emitterVertex.bRec.wo = emitterVertex.bRec.dg.toLocal(-direction);
	const Spectrum emitterBsdf = emitterVertex.mat->bsdf.f(emitterVertex.bRec);
	const float cosLight = fabsf(Frame::cosTheta(emitterVertex.bRec.wo));
	const float lightBsdfDirPdfW = pdf(*emitterVertex.mat, emitterVertex.bRec);
	const float lightBsdfRevPdfW = revPdf(*emitterVertex.mat, emitterVertex.bRec);

	if (cameraBsdf.isZero() || emitterBsdf.isZero())
		return Spectrum(0.0f);

	const float cameraBsdfDirPdfA = PdfWtoA(cameraBsdfDirPdfW, dist2, cosLight);
	const float lightBsdfDirPdfA = PdfWtoA(lightBsdfDirPdfW, dist2, cosCamera);

	const float wLight = Mis(cameraBsdfDirPdfA) * (
		mMisVmWeightFactor + emitterVertex.dVCM + emitterVertex.dVC * Mis(lightBsdfRevPdfW));

	// Partial eye sub-path MIS weight [tech. rep. (41)]
	const float wCamera = Mis(lightBsdfDirPdfA) * (
		mMisVmWeightFactor + cameraState.dVCM + cameraState.dVC * Mis(cameraBsdfRevPdfW));

	// Full path MIS weight [tech. rep. (37)]
	const float misWeight = use_mis ? 1.f / (wLight + 1.f + wCamera) : 1;

	const float geometryTerm = 1.0f / dist2;
	const Spectrum contrib = (misWeight * geometryTerm) * cameraBsdf * emitterBsdf;
	if (contrib.isZero() || !::V(bRec.dg.P, emitterVertex.dg.P))
		return Spectrum(0.0f);
	return contrib;
}