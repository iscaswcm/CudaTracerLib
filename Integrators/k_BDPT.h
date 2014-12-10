#pragma once

#include "..\Kernel\k_Tracer.h"
#include "..\Base\CudaRandom.h"

class k_BDPT : public k_ProgressiveTracer
{
public:
	k_BDPT(bool direct = false)
	{
	}
	virtual void Debug(e_Image* I, int2 pixel);
protected:
	virtual void DoRender(e_Image* I);
};