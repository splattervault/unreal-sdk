// Copyright (c) SplatterVault. Licensed under the MIT License.

using System.IO;
using UnrealBuildTool;

public class SplatterVault : ModuleRules
{
	public SplatterVault(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Each file keeps its helpers in an anonymous namespace; no unity build, so they never collide.
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"HTTP",
			"Json",
			"JsonUtilities"
		});

		bool bOlderEngine = Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 5;
		if (Target.Platform == UnrealTargetPlatform.Win64 && Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 3)
		{
			// UE 5.3 with MSVC 14.40 or newer (Visual Studio 17.10+): Experimental/ConcurrentLinearAllocator.h tests
			// __has_feature, which MSVC does not have, and the build fails in engine headers (C4668/C4067). The ASan
			// header probe is not needed here: turn it off for this module.
			PrivateDefinitions.Add("PLATFORM_HAS_ASAN_INCLUDE=0");
		}
		if (bOlderEngine)
		{
			// UE 5.3-5.5: the shared engine PCH (Engine.h) carries engine-header warnings (deprecated FBoneMirrorInfo)
			// and, on 5.3 with newer MSVC, the error above. This small module compiles without it.
			PCHUsage = PCHUsageMode.NoPCHs;
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// DPAPI (CryptProtectData) for the token store, BCryptGenRandom for random bytes.
			PublicSystemLibraries.Add("crypt32.lib");
			PublicSystemLibraries.Add("bcrypt.lib");
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// Adds the Android Keystore helper (Java) to the APK.
			AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(ModuleDirectory, "SplatterVault_UPL.xml"));
		}
	}
}
