#pragma once

#include <MathTypes.h>

struct e_TriIntersectorData;
struct PositionSamplingRecord;
template<typename H, typename D> class e_BufferReference;
template<typename T> class e_Stream;

struct CUDA_ALIGN(16) ShapeSet
{
	CUDA_ALIGN(16) struct triData
	{
		CUDA_ALIGN(16) Vec3f p[3];
		float area;
		e_Variable<e_TriIntersectorData> iDat;
		
		AABB box() const;
		void Recalculate(const float4x4& mat);
	};
public:
	ShapeSet(){}
	ShapeSet(e_BufferReference<e_TriIntersectorData, e_TriIntersectorData>* indices, unsigned int indexCount, float4x4& mat, e_Stream<char>* buffer);
	CUDA_FUNC_IN float Area() const { return sumArea; }
	CUDA_DEVICE CUDA_HOST void SamplePosition(PositionSamplingRecord& pRec, const Vec2f& spatialSample) const;
	CUDA_FUNC_IN float Pdf() const
	{
		return 1.0f / sumArea;
	}
	CUDA_FUNC_IN AABB getBox() const
	{
		AABB b = AABB::Identity();
		for (unsigned int i = 0; i < count; i++)
			b = b.Extend(triangles[i].box());
		return b;
	}
	void Recalculate(const float4x4& mat, e_Stream<char>* buffer);

	CUDA_FUNC_IN unsigned int numTriangles() const
	{
		return count;
	}
	CUDA_FUNC_IN const triData& getTriangle(unsigned int index) const
	{
		return triangles[index];
	}
private:
	e_Variable<float> areaDistribution;
	e_Variable<triData> triangles;
	float sumArea;
	unsigned int count;
};