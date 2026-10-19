// pi/engine/tests/shim/circle/exceptionhandler.h - the base class CEngineExceptionHandler derives from (declared only).
#pragma once
#include <circle/types.h>
struct TAbortFrame { u64 esr_el1, spsr_el1, x30, elr_el1, sp_el0, sp_el1, far_el1; };
class CExceptionHandler
{
public:
	virtual ~CExceptionHandler (void) {}
	virtual void Throw (unsigned, TAbortFrame *) {}
};
