// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using System.Linq;
using UnrealBuildTool;

public class MultiServerReplicationEx : ModuleRules
{
	public MultiServerReplicationEx(ReadOnlyTargetRules Target) : base(Target)
    {
        PrivateDependencyModuleNames.AddRange(
			new string[] {
                "Core",
                "CoreUObject",
                "CoreOnline",
                "Engine",
				"NetCore",
				"MultiServerConfigurationEx",
				"HTTP",
				"HTTPServer"
            }
        );

        PublicDependencyModuleNames.AddRange(
            new string[] {
				"OnlineSubsystem",
				"OnlineSubsystemUtils"
			}
		);

		// UE_WITH_REMOTE_OBJECT_HANDLE builds need access to CoreUObject Internal
		// headers (e.g. UObject/UObjectMigrationContext.h) which are not exposed
		// to external modules by default.
		if (Target.GlobalDefinitions.Contains("UE_WITH_REMOTE_OBJECT_HANDLE=1"))
		{
			PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "CoreUObject", "Internal"));
		}
    }
}
