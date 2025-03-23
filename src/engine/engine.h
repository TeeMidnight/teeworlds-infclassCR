/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_ENGINE_H
#define ENGINE_ENGINE_H

#include "kernel.h"
#include <engine/shared/jobs.h>

class IEngine : public IInterface
{
	MACRO_INTERFACE("engine", 0)

protected:
	class CJobPool m_JobPool;

public:
	virtual ~IEngine() = default;

	virtual void Init() = 0;
	virtual void AddJob(std::shared_ptr<IJob> pJob) = 0;
};

extern IEngine *CreateEngine(const char *pAppname, int Jobs);
extern IEngine *CreateTestEngine(const char *pAppname, int Jobs);

#endif
