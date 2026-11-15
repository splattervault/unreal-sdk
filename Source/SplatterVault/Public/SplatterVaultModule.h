// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/** The SDK's log category. Its lines never contain a token, a secret, a proof, a password or a stored value. */
SPLATTERVAULT_API DECLARE_LOG_CATEGORY_EXTERN(LogSplatterVault, Log, All);

/** The SplatterVault runtime module. */
class FSplatterVaultModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
