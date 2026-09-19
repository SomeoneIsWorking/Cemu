#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

namespace LatteFrameHooks
{
	namespace
	{
		Observer* s_observer = nullptr;
	}

	void SetObserver(Observer* observer)
	{
		s_observer = observer;
	}

	Observer* GetObserver()
	{
		return s_observer;
	}

} // namespace LatteFrameHooks
