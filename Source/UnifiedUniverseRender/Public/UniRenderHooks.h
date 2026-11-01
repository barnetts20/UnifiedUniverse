#pragma once

#include "CoreMinimal.h"

/** The engine hook surface (D-006): everything version-sensitive is in UniRenderHooks.cpp. */
namespace UniRenderHooks
{
	/** Game thread. Creates the scene view extension once (after engine init). */
	UNIFIEDUNIVERSERENDER_API void EnsureRegistered();

	/** Game thread. The extension is active only while a universe is enabled. */
	UNIFIEDUNIVERSERENDER_API void SetActive(bool bActive);
}
