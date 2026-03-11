#include "AssetHiveModule.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "IContentBrowserSingleton.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "ToolMenus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

IMPLEMENT_MODULE(FAssetHiveModule, AssetHive)

static FString GetBridgeHeartbeatPath()
{
    return FPaths::Combine(FPlatformProcess::UserDir(), TEXT("AssetHive"), TEXT("bridge-heartbeat.json"));
}

static FString GetImportSignalPath()
{
    return FPaths::Combine(FPlatformProcess::UserDir(), TEXT("AssetHive"), TEXT("import-signal.json"));
}

void FAssetHiveModule::StartupModule()
{
    SetupStyle();
    UpdateConnectionState();
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FAssetHiveModule::TickConnection), 2.0f);
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAssetHiveModule::RegisterMenus));
}

void FAssetHiveModule::ShutdownModule()
{
    if (TickHandle.IsValid()) {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }
    if (UToolMenus* ToolMenus = UToolMenus::TryGet()) {
        ToolMenus->UnregisterOwner(this);
    }
    TeardownStyle();
}

void FAssetHiveModule::SetupStyle()
{
    if (StyleSet.IsValid()) {
        return;
    }
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AssetHive"));
    if (!Plugin.IsValid()) {
        return;
    }
    StyleSet = MakeShared<FSlateStyleSet>(TEXT("AssetHiveStyle"));
    StyleSet->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
    StyleSet->Set(TEXT("AssetHive.ConnectionIcon"), new FSlateImageBrush(StyleSet->RootToContentDir(TEXT("Icon128.png")), FVector2D(14.0f, 14.0f)));
    FSlateStyleRegistry::RegisterSlateStyle(*StyleSet.Get());
}

void FAssetHiveModule::TeardownStyle()
{
    if (!StyleSet.IsValid()) {
        return;
    }
    FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet.Get());
    StyleSet.Reset();
}

void FAssetHiveModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar"));
    if (!Menu) {
        return;
    }
    FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("AssetHive"));
    const FToolMenuEntry Entry = FToolMenuEntry::InitWidget(
        TEXT("AssetHiveConnectionStatus"),
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(6.0f, 0.0f, 6.0f, 0.0f)
        [
            SNew(SImage)
            .Image_Lambda([this]() -> const FSlateBrush* {
                return StyleSet.IsValid() ? StyleSet->GetBrush(TEXT("AssetHive.ConnectionIcon")) : nullptr;
            })
            .ColorAndOpacity_Lambda([this]() {
                return bConnectedToAssetHive ? FLinearColor(0.18f, 0.82f, 0.35f, 1.0f) : FLinearColor(0.86f, 0.24f, 0.24f, 1.0f);
            })
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text_Lambda([this]() {
                return bConnectedToAssetHive ? FText::FromString(TEXT("AssetHive Connected")) : FText::FromString(TEXT("AssetHive Disconnected"));
            })
            .ColorAndOpacity_Lambda([this]() {
                return bConnectedToAssetHive ? FLinearColor(0.80f, 1.0f, 0.86f, 1.0f) : FLinearColor(1.0f, 0.78f, 0.78f, 1.0f);
            })
        ],
        FText::FromString(TEXT("AssetHive Connection Status"))
    );
    Section.AddEntry(Entry);
}

bool FAssetHiveModule::TickConnection(float DeltaTime)
{
    UpdateConnectionState();
    ConsumeImportSignal();
    return true;
}

void FAssetHiveModule::UpdateConnectionState()
{
    const FString HeartbeatPath = GetBridgeHeartbeatPath();
    FString HeartbeatContent;
    if (FFileHelper::LoadFileToString(HeartbeatContent, *HeartbeatPath)) {
        TSharedPtr<FJsonObject> HeartbeatJson;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HeartbeatContent);
        if (FJsonSerializer::Deserialize(Reader, HeartbeatJson) && HeartbeatJson.IsValid()) {
            double TimestampMs = 0.0;
            if (HeartbeatJson->TryGetNumberField(TEXT("timestamp"), TimestampMs)) {
                const int64 UtcNowMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
                if (UtcNowMs - static_cast<int64>(TimestampMs) <= 12000) {
                    bConnectedToAssetHive = true;
                    return;
                }
            }
        }
    }

    int32 ReturnCode = 1;
    FString StandardOutput;
    FString StandardError;
    const FString Arguments = TEXT("-NoProfile -NonInteractive -Command \"Get-Process AssetHive,'AssetHive.r',ArkHive,electron -ErrorAction SilentlyContinue | Select-Object -ExpandProperty ProcessName\"");
    FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *Arguments, &ReturnCode, &StandardOutput, &StandardError);
    const FString LowerOutput = StandardOutput.ToLower();
    bConnectedToAssetHive = LowerOutput.Contains(TEXT("assethive")) || LowerOutput.Contains(TEXT("arkhive")) || LowerOutput.Contains(TEXT("electron"));
}

void FAssetHiveModule::ConsumeImportSignal()
{
    const FString SignalPath = GetImportSignalPath();
    FString SignalContent;
    if (!FFileHelper::LoadFileToString(SignalContent, *SignalPath)) {
        return;
    }
    TSharedPtr<FJsonObject> SignalJson;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SignalContent);
    if (!FJsonSerializer::Deserialize(Reader, SignalJson) || !SignalJson.IsValid()) {
        return;
    }
    double TimestampMs = 0.0;
    if (!SignalJson->TryGetNumberField(TEXT("timestamp"), TimestampMs)) {
        return;
    }
    const int64 Timestamp = static_cast<int64>(TimestampMs);
    if (Timestamp <= LastImportSignalTimestamp) {
        return;
    }
    FString FolderPath;
    if (!SignalJson->TryGetStringField(TEXT("folder"), FolderPath) || FolderPath.IsEmpty()) {
        return;
    }
    LastImportSignalTimestamp = Timestamp;
    if (IContentBrowserSingleton* Browser = &FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get()) {
        TArray<FString> Folders;
        Folders.Add(FolderPath);
        Browser->SyncBrowserToFolders(Folders);
    }
}
