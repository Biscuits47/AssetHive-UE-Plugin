using UnrealBuildTool;

public class AssetHive : ModuleRules
{
    public AssetHive(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects",
            "Slate",
            "SlateCore"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetTools",
            "AssetRegistry",
            "ContentBrowser",
            "EditorFramework",
            "Json",
            "JsonUtilities",
            "LevelEditor",
            "MaterialEditor",
            "ToolMenus",
            "UnrealEd"
        });
    }
}
