// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SplatterVaultSettings.h"

USplatterVaultSettings::USplatterVaultSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("SplatterVault");
}

FName USplatterVaultSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}
