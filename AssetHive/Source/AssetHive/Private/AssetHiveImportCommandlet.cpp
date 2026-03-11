#include "AssetHiveImportCommandlet.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxImportUI.h"
#include "Factories/TextureFactory.h"
#include "ImageUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PixelFormat.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

UAssetHiveImportCommandlet::UAssetHiveImportCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}

static FString MakeSafeObjectName(const FString& Name)
{
    FString SafeName = Name;
    SafeName.ReplaceInline(TEXT(" "), TEXT("_"));
    SafeName.ReplaceInline(TEXT("-"), TEXT("_"));
    SafeName.ReplaceInline(TEXT("."), TEXT("_"));
    return SafeName;
}

static FString NormalizePathLower(const FString& Value)
{
    FString Result = Value.Replace(TEXT("\\"), TEXT("/"));
    return Result.ToLower();
}

static FString DetectTextureSlot(const FString& SourceFile)
{
    const FString Name = FPaths::GetBaseFilename(SourceFile).ToLower();
    if (Name.Contains(TEXT("albedo")) || Name.Contains(TEXT("basecolor")) || Name.Contains(TEXT("base_color")) || Name.Contains(TEXT("diffuse")) || Name.Contains(TEXT("color"))) return TEXT("albedo");
    if (Name.Contains(TEXT("ao")) || Name.Contains(TEXT("ambientocclusion")) || Name.Contains(TEXT("ambient_occlusion"))) return TEXT("ao");
    if (Name.Contains(TEXT("normal")) || Name.Contains(TEXT("nrm")) || Name.Contains(TEXT("nor"))) return TEXT("normal");
    if (Name.Contains(TEXT("roughness")) || Name.Contains(TEXT("rough"))) return TEXT("roughness");
    if (Name.Contains(TEXT("metalness")) || Name.Contains(TEXT("metallic")) || Name.Contains(TEXT("metal"))) return TEXT("metalness");
    if (Name.Contains(TEXT("displacement")) || Name.Contains(TEXT("height"))) return TEXT("displacement");
    if (Name.Contains(TEXT("fuzz"))) return TEXT("fuzz");
    if (Name.Contains(TEXT("ordp")) || Name.Contains(TEXT("orm"))) return TEXT("ordp");
    if (Name.Contains(TEXT("specular")) || Name.Contains(TEXT("spec"))) return TEXT("specular");
    if (Name.Contains(TEXT("opacity")) || Name.Contains(TEXT("alpha")) || Name.Contains(TEXT("transparency"))) return TEXT("opacity");
    return TEXT("");
}

static FString DetectModelSuffix(const FString& SourceFile)
{
    const FString Name = FPaths::GetBaseFilename(SourceFile).ToLower();
    if (Name.Contains(TEXT("highpoly")) || Name.Contains(TEXT("_high")) || Name.Contains(TEXT("-high")) || Name.EndsWith(TEXT("high"))) return TEXT("High");
    if (Name.Contains(TEXT("lod0"))) return TEXT("Lod0");
    if (Name.Contains(TEXT("lod1"))) return TEXT("Lod1");
    if (Name.Contains(TEXT("lod2"))) return TEXT("Lod2");
    if (Name.Contains(TEXT("lod3"))) return TEXT("Lod3");
    if (Name.Contains(TEXT("ztool")) || Name.EndsWith(TEXT(".ztl"))) return TEXT("Ztool");
    return TEXT("Mesh");
}

static FString ToSlotSuffix(const FString& SlotName)
{
    if (SlotName.IsEmpty()) {
        return TEXT("Texture");
    }
    FString Result = SlotName.ToLower();
    Result[0] = FChar::ToUpper(Result[0]);
    return Result;
}

static void AppendImportedObjects(UAssetImportTask* Task, TArray<UObject*>& OutObjects)
{
    if (!Task) {
        return;
    }
    for (const FString& ImportedPath : Task->ImportedObjectPaths) {
        if (UObject* ImportedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ImportedPath)) {
            OutObjects.Add(ImportedObject);
        }
    }
}

static bool SavePackageForObject(UObject* AssetObject)
{
    if (!AssetObject) {
        return false;
    }
    UPackage* Package = AssetObject->GetOutermost();
    if (!Package) {
        return false;
    }
    const FString PackageName = Package->GetName();
    const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Package, AssetObject, *FileName, SaveArgs);
}

static void EmitProgress(int32 Percent, const FString& Stage)
{
    const int32 Clamped = FMath::Clamp(Percent, 0, 100);
    UE_LOG(LogTemp, Display, TEXT("[AssetHiveProgress]%d|%s"), Clamped, *Stage);
}

static void WriteImportSignal(const FString& FolderPath)
{
    if (FolderPath.IsEmpty()) {
        return;
    }
    const FString SignalPath = FPaths::Combine(FPlatformProcess::UserDir(), TEXT("AssetHive"), TEXT("import-signal.json"));
    const FString SignalDir = FPaths::GetPath(SignalPath);
    IFileManager::Get().MakeDirectory(*SignalDir, true);
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("folder"), FolderPath);
    Root->SetNumberField(TEXT("timestamp"), static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp() * 1000));
    FString OutJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
    FFileHelper::SaveStringToFile(OutJson, *SignalPath);
}

struct FTexturePixels
{
    int32 Width = 0;
    int32 Height = 0;
    TArray<FColor> Pixels;
};

static bool ReadTexturePixels(UTexture2D* Texture, FTexturePixels& OutPixels)
{
    if (!Texture) {
        return false;
    }

    if (Texture->Source.IsValid()) {
        const int32 Width = Texture->Source.GetSizeX();
        const int32 Height = Texture->Source.GetSizeY();
        if (Width <= 0 || Height <= 0) {
            return false;
        }

        TArray64<uint8> RawData;
        if (!Texture->Source.GetMipData(RawData, 0)) {
            return false;
        }

        const ETextureSourceFormat Format = Texture->Source.GetFormat();
        const int32 PixelCount = Width * Height;
        OutPixels.Width = Width;
        OutPixels.Height = Height;
        OutPixels.Pixels.SetNum(PixelCount);

        if (Format == TSF_BGRA8) {
            if (RawData.Num() < PixelCount * 4) {
                return false;
            }
            FMemory::Memcpy(OutPixels.Pixels.GetData(), RawData.GetData(), PixelCount * sizeof(FColor));
            return true;
        }
        if (Format == TSF_G8) {
            if (RawData.Num() < PixelCount) {
                return false;
            }
            for (int32 Index = 0; Index < PixelCount; Index++) {
                const uint8 Value = RawData[Index];
                OutPixels.Pixels[Index] = FColor(Value, Value, Value, 255);
            }
            return true;
        }
    }
    const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
    if (!PlatformData || PlatformData->Mips.Num() <= 0) {
        return false;
    }
    const FTexture2DMipMap& Mip = PlatformData->Mips[0];
    const int32 Width = Mip.SizeX;
    const int32 Height = Mip.SizeY;
    if (Width <= 0 || Height <= 0) {
        return false;
    }
    const int32 PixelCount = Width * Height;
    const int64 RequiredRGBA = static_cast<int64>(PixelCount) * 4;
    OutPixels.Width = Width;
    OutPixels.Height = Height;
    OutPixels.Pixels.SetNum(PixelCount);

    const EPixelFormat PixelFormat = PlatformData->PixelFormat;
    const void* RawPtr = Mip.BulkData.LockReadOnly();
    if (!RawPtr) {
        Mip.BulkData.Unlock();
        return false;
    }
    const int64 RawSize = Mip.BulkData.GetBulkDataSize();
    bool bOk = false;
    if ((PixelFormat == PF_B8G8R8A8 || PixelFormat == PF_R8G8B8A8) && RawSize >= RequiredRGBA) {
        const uint8* Bytes = static_cast<const uint8*>(RawPtr);
        for (int32 Index = 0; Index < PixelCount; Index++) {
            const int32 Offset = Index * 4;
            if (PixelFormat == PF_B8G8R8A8) {
                OutPixels.Pixels[Index] = FColor(Bytes[Offset + 2], Bytes[Offset + 1], Bytes[Offset], Bytes[Offset + 3]);
            } else {
                OutPixels.Pixels[Index] = FColor(Bytes[Offset], Bytes[Offset + 1], Bytes[Offset + 2], Bytes[Offset + 3]);
            }
        }
        bOk = true;
    } else if ((PixelFormat == PF_G8 || PixelFormat == PF_R8) && RawSize >= PixelCount) {
        const uint8* Bytes = static_cast<const uint8*>(RawPtr);
        for (int32 Index = 0; Index < PixelCount; Index++) {
            const uint8 Value = Bytes[Index];
            OutPixels.Pixels[Index] = FColor(Value, Value, Value, 255);
        }
        bOk = true;
    }
    Mip.BulkData.Unlock();
    return bOk;
}

static uint8 SampleChannel(const FTexturePixels* Pixels, float U, float V, int32 ChannelIndex, uint8 DefaultValue)
{
    if (!Pixels || Pixels->Width <= 0 || Pixels->Height <= 0 || Pixels->Pixels.IsEmpty()) {
        return DefaultValue;
    }
    const int32 X = FMath::Clamp(FMath::FloorToInt(U * (Pixels->Width - 1)), 0, Pixels->Width - 1);
    const int32 Y = FMath::Clamp(FMath::FloorToInt(V * (Pixels->Height - 1)), 0, Pixels->Height - 1);
    const FColor& Pixel = Pixels->Pixels[Y * Pixels->Width + X];
    if (ChannelIndex == 0) return Pixel.R;
    if (ChannelIndex == 1) return Pixel.G;
    if (ChannelIndex == 2) return Pixel.B;
    return Pixel.A;
}

static UTexture2D* CreatePackedMaskTexture(
    const FString& AssetFolder,
    const FString& AssetName,
    UTexture2D* AOTexture,
    int32 AOChannel,
    UTexture2D* RoughnessTexture,
    int32 RoughnessChannel,
    UTexture2D* DisplacementTexture,
    int32 DisplacementChannel,
    UTexture2D* SizeRefA,
    UTexture2D* SizeRefB
)
{
    const bool HasAOInput = AOTexture != nullptr;
    const bool HasRoughnessInput = RoughnessTexture != nullptr;
    const bool HasDisplacementInput = DisplacementTexture != nullptr;
    FTexturePixels AOPixels;
    FTexturePixels RoughnessPixels;
    FTexturePixels DisplacementPixels;
    const bool HasAO = ReadTexturePixels(AOTexture, AOPixels);
    const bool HasRoughness = ReadTexturePixels(RoughnessTexture, RoughnessPixels);
    const bool HasDisplacement = ReadTexturePixels(DisplacementTexture, DisplacementPixels);
    int32 Width = 0;
    int32 Height = 0;
    if (HasAO) {
        Width = AOPixels.Width;
        Height = AOPixels.Height;
    } else if (HasRoughness) {
        Width = RoughnessPixels.Width;
        Height = RoughnessPixels.Height;
    } else if (HasDisplacement) {
        Width = DisplacementPixels.Width;
        Height = DisplacementPixels.Height;
    } else {
        FTexturePixels RefPixels;
        if (ReadTexturePixels(SizeRefA, RefPixels) || ReadTexturePixels(SizeRefB, RefPixels)) {
            Width = RefPixels.Width;
            Height = RefPixels.Height;
        } else {
            Width = 1024;
            Height = 1024;
        }
    }

    const FString TextureAssetName = FString::Printf(TEXT("T_%s_M"), *AssetName);
    const FString PackagePath = AssetFolder / TextureAssetName;
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package) {
        return nullptr;
    }

    UTexture2D* PackedTexture = NewObject<UTexture2D>(Package, *TextureAssetName, RF_Public | RF_Standalone);
    if (!PackedTexture) {
        return nullptr;
    }

    PackedTexture->Source.Init(Width, Height, 1, 1, TSF_BGRA8);
    uint8* DestData = PackedTexture->Source.LockMip(0);
    for (int32 Y = 0; Y < Height; Y++) {
        for (int32 X = 0; X < Width; X++) {
            const float U = Width > 1 ? static_cast<float>(X) / static_cast<float>(Width - 1) : 0.0f;
            const float V = Height > 1 ? static_cast<float>(Y) / static_cast<float>(Height - 1) : 0.0f;
            const uint8 AOFallback = HasAOInput ? static_cast<uint8>(0) : static_cast<uint8>(255);
            const uint8 RoughnessFallback = HasRoughnessInput ? static_cast<uint8>(0) : static_cast<uint8>(204);
            const uint8 DisplacementFallback = HasDisplacementInput ? static_cast<uint8>(0) : static_cast<uint8>(128);
            const uint8 AOValue = SampleChannel(HasAO ? &AOPixels : nullptr, U, V, AOChannel, AOFallback);
            const uint8 RoughnessValue = SampleChannel(HasRoughness ? &RoughnessPixels : nullptr, U, V, RoughnessChannel, RoughnessFallback);
            const uint8 DisplacementValue = SampleChannel(HasDisplacement ? &DisplacementPixels : nullptr, U, V, DisplacementChannel, DisplacementFallback);
            const int32 DestIndex = (Y * Width + X) * 4;
            DestData[DestIndex + 0] = DisplacementValue;
            DestData[DestIndex + 1] = RoughnessValue;
            DestData[DestIndex + 2] = AOValue;
            DestData[DestIndex + 3] = 255;
        }
    }
    PackedTexture->Source.UnlockMip(0);
    PackedTexture->CompressionSettings = TC_Masks;
    PackedTexture->CompressionNoAlpha = true;
    PackedTexture->SRGB = false;
    PackedTexture->PostEditChange();
    PackedTexture->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(PackedTexture);
    SavePackageForObject(PackedTexture);
    return PackedTexture;
}

static UMaterialInstanceConstant* CreateAssetMaterialInstance(
    const FString& AssetFolder,
    const FString& AssetName,
    UTexture* AlbedoTexture,
    UTexture* NormalTexture,
    UTexture* MaskTexture,
    UTexture* FuzzTexture
)
{
    const bool HasFuzz = FuzzTexture != nullptr;
    const TCHAR* ParentPath = HasFuzz
        ? TEXT("/Game/Common/MaterialInstance/MMI_GeneralMat_Fuzz.MMI_GeneralMat_Fuzz")
        : TEXT("/Game/Common/MaterialInstance/MMI_GeneralMat.MMI_GeneralMat");
    UMaterialInterface* ParentMaterial = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, ParentPath));
    if (!ParentMaterial) {
        UE_LOG(LogTemp, Warning, TEXT("Missing parent material: %s"), ParentPath);
        return nullptr;
    }

    const FString MaterialAssetName = FString::Printf(TEXT("MI_%s"), *AssetName);
    const FString MaterialPackagePath = AssetFolder / MaterialAssetName;
    UPackage* MaterialPackage = CreatePackage(*MaterialPackagePath);
    UMaterialInstanceConstant* MaterialInstance = FindObject<UMaterialInstanceConstant>(MaterialPackage, *MaterialAssetName);
    const bool bIsNew = MaterialInstance == nullptr;
    if (!MaterialInstance) {
        MaterialInstance = NewObject<UMaterialInstanceConstant>(MaterialPackage, *MaterialAssetName, RF_Public | RF_Standalone);
    }
    if (!MaterialInstance) {
        return nullptr;
    }
    MaterialInstance->SetParentEditorOnly(ParentMaterial);

    if (AlbedoTexture) {
        UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, FName(TEXT("Albedo")), AlbedoTexture);
    }
    if (MaskTexture) {
        UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, FName(TEXT("Mask")), MaskTexture);
    }
    if (NormalTexture) {
        UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, FName(TEXT("Normal")), NormalTexture);
    }
    if (FuzzTexture) {
        UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, FName(TEXT("fuzzmap")), FuzzTexture);
    }

    MaterialInstance->PostEditChange();
    MaterialInstance->MarkPackageDirty();
    if (bIsNew) {
        FAssetRegistryModule::AssetCreated(MaterialInstance);
    }
    SavePackageForObject(MaterialInstance);
    return MaterialInstance;
}

int32 UAssetHiveImportCommandlet::Main(const FString& Params)
{
    FScopedSlowTask SlowTask(100.0f, FText::FromString(TEXT("AssetHive 导入进行中")));
    SlowTask.MakeDialog(true);
    float LastProgress = 0.0f;
    const auto SetStageProgress = [&SlowTask, &LastProgress](float Target, const FString& Stage) {
        const float Clamped = FMath::Clamp(Target, 0.0f, 100.0f);
        const float Delta = FMath::Max(0.0f, Clamped - LastProgress);
        SlowTask.EnterProgressFrame(Delta, FText::FromString(Stage));
        LastProgress = Clamped;
        EmitProgress(static_cast<int32>(Clamped), Stage);
    };
    SetStageProgress(2.0f, TEXT("读取导入任务"));
    FString JobFilePath;
    if (!FParse::Value(*Params, TEXT("Job="), JobFilePath)) {
        UE_LOG(LogTemp, Error, TEXT("Missing -Job argument."));
        return 1;
    }

    FString JobContent;
    if (!FFileHelper::LoadFileToString(JobContent, *JobFilePath)) {
        UE_LOG(LogTemp, Error, TEXT("Failed to read job file: %s"), *JobFilePath);
        return 1;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JobContent);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) {
        UE_LOG(LogTemp, Error, TEXT("Invalid job json: %s"), *JobFilePath);
        return 1;
    }

    FString DestinationPath = TEXT("/Game/AssetHive");
    Root->TryGetStringField(TEXT("destinationPath"), DestinationPath);

    const TArray<TSharedPtr<FJsonValue>>* AssetsJson = nullptr;
    if (!Root->TryGetArrayField(TEXT("assets"), AssetsJson) || AssetsJson == nullptr) {
        UE_LOG(LogTemp, Error, TEXT("No assets in job file."));
        return 1;
    }

    FAssetToolsModule& AssetToolsModule = FAssetToolsModule::GetModule();

    const int32 AssetCount = AssetsJson->Num();
    int32 AssetIndex = 0;
    for (const TSharedPtr<FJsonValue>& AssetValue : *AssetsJson) {
        if (!AssetValue.IsValid() || AssetValue->Type != EJson::Object) {
            continue;
        }
        const int32 AssetBaseProgress = 10 + (AssetIndex * 80) / FMath::Max(1, AssetCount);
        const int32 AssetEndProgress = 10 + ((AssetIndex + 1) * 80) / FMath::Max(1, AssetCount);
        SetStageProgress(static_cast<float>(AssetBaseProgress), FString::Printf(TEXT("处理资产 %d/%d"), AssetIndex + 1, AssetCount));

        TSharedPtr<FJsonObject> AssetObject = AssetValue->AsObject();
        FString AssetName = TEXT("AssetHiveAsset");
        FString AssetId = TEXT("");
        AssetObject->TryGetStringField(TEXT("name"), AssetName);
        AssetObject->TryGetStringField(TEXT("id"), AssetId);
        if (AssetId.IsEmpty()) {
            AssetId = TEXT("UnknownId");
        }
        const FString SafeAssetName = MakeSafeObjectName(AssetName);
        const FString SafeAssetId = MakeSafeObjectName(AssetId);
        const FString AssetStem = FString::Printf(TEXT("%s_%s"), *SafeAssetName, *SafeAssetId);
        const FString AssetFolder = DestinationPath / SafeAssetName;

        TMap<FString, FString> SourceTextureSlotMap;
        const TArray<TSharedPtr<FJsonValue>>* TextureSlots = nullptr;
        if (AssetObject->TryGetArrayField(TEXT("textureSlots"), TextureSlots) && TextureSlots != nullptr) {
            for (const TSharedPtr<FJsonValue>& SlotValue : *TextureSlots) {
                if (!SlotValue.IsValid() || SlotValue->Type != EJson::Object) {
                    continue;
                }
                const TSharedPtr<FJsonObject> SlotObject = SlotValue->AsObject();
                if (!SlotObject.IsValid()) {
                    continue;
                }
                FString SourceFile;
                FString SlotName;
                SlotObject->TryGetStringField(TEXT("file"), SourceFile);
                SlotObject->TryGetStringField(TEXT("slot"), SlotName);
                if (!SourceFile.IsEmpty() && !SlotName.IsEmpty()) {
                    SourceTextureSlotMap.Add(NormalizePathLower(SourceFile), SlotName.ToLower());
                }
            }
        }

        TArray<UStaticMesh*> ImportedMeshes;
        TMap<FString, UTexture*> TextureBySlot;
        TMap<FString, FString> SourceTextureBySlot;

        const TArray<TSharedPtr<FJsonValue>>* ModelFiles = nullptr;
        if (AssetObject->TryGetArrayField(TEXT("modelFiles"), ModelFiles) && ModelFiles != nullptr) {
            for (const TSharedPtr<FJsonValue>& FileValue : *ModelFiles) {
                if (!FileValue.IsValid() || FileValue->Type != EJson::String) {
                    continue;
                }
                const FString SourceFile = FileValue->AsString();
                if (!FPaths::FileExists(SourceFile)) {
                    UE_LOG(LogTemp, Warning, TEXT("Source file missing: %s"), *SourceFile);
                    continue;
                }
                UAssetImportTask* Task = NewObject<UAssetImportTask>();
                Task->Filename = SourceFile;
                Task->DestinationPath = AssetFolder;
                Task->DestinationName = FString::Printf(TEXT("SM_%s_%s"), *AssetStem, *DetectModelSuffix(SourceFile));
                Task->bReplaceExisting = true;
                Task->bAutomated = true;
                Task->bSave = true;
                UFbxImportUI* ImportOptions = NewObject<UFbxImportUI>();
                ImportOptions->bImportMaterials = false;
                ImportOptions->bImportTextures = false;
                ImportOptions->bImportAnimations = false;
                ImportOptions->bAutomatedImportShouldDetectType = false;
                ImportOptions->MeshTypeToImport = FBXIT_StaticMesh;
                Task->Options = ImportOptions;
                AssetToolsModule.Get().ImportAssetTasks({ Task });

                TArray<UObject*> ImportedObjects;
                AppendImportedObjects(Task, ImportedObjects);
                for (UObject* ImportedObject : ImportedObjects) {
                    if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(ImportedObject)) {
                        ImportedMeshes.Add(StaticMesh);
                    }
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* TextureFiles = nullptr;
        if (AssetObject->TryGetArrayField(TEXT("textureFiles"), TextureFiles) && TextureFiles != nullptr) {
            for (const TSharedPtr<FJsonValue>& FileValue : *TextureFiles) {
                if (!FileValue.IsValid() || FileValue->Type != EJson::String) {
                    continue;
                }
                const FString SourceFile = FileValue->AsString();
                if (!FPaths::FileExists(SourceFile)) {
                    UE_LOG(LogTemp, Warning, TEXT("Source file missing: %s"), *SourceFile);
                    continue;
                }
                const FString SourceKey = NormalizePathLower(SourceFile);
                const FString SlotName = SourceTextureSlotMap.Contains(SourceKey)
                    ? SourceTextureSlotMap[SourceKey]
                    : DetectTextureSlot(SourceFile);
                if (!SlotName.IsEmpty() && !SourceTextureBySlot.Contains(SlotName)) {
                    SourceTextureBySlot.Add(SlotName, SourceFile);
                }
                if (!(SlotName == TEXT("albedo") || SlotName == TEXT("normal") || SlotName == TEXT("fuzz"))) {
                    continue;
                }
                UAssetImportTask* Task = NewObject<UAssetImportTask>();
                Task->Filename = SourceFile;
                Task->DestinationPath = AssetFolder;
                Task->DestinationName = FString::Printf(TEXT("T_%s_%s"), *AssetStem, *ToSlotSuffix(SlotName));
                Task->bReplaceExisting = true;
                Task->bAutomated = true;
                Task->bSave = true;
                
                if (SlotName == TEXT("displacement")) {
                    UTextureFactory* Factory = NewObject<UTextureFactory>();
                    Factory->CompressionSettings = TC_Displacementmap;
                    Factory->SRGB = false;
                    Task->Factory = Factory;
                }
                
                AssetToolsModule.Get().ImportAssetTasks({ Task });

                TArray<UObject*> ImportedObjects;
                AppendImportedObjects(Task, ImportedObjects);
                for (UObject* ImportedObject : ImportedObjects) {
                    if (UTexture* Texture = Cast<UTexture>(ImportedObject)) {
                        if (!SlotName.IsEmpty() && !TextureBySlot.Contains(SlotName)) {
                            TextureBySlot.Add(SlotName, Texture);
                        }
                    }
                }
            }
        }

        SetStageProgress(static_cast<float>(FMath::Clamp(AssetBaseProgress + 25, 0, 99)), FString::Printf(TEXT("合成 M 贴图: %s"), *AssetName));
        UTexture2D* AOSourceTexture = SourceTextureBySlot.Contains(TEXT("ao")) ? FImageUtils::ImportFileAsTexture2D(SourceTextureBySlot[TEXT("ao")]) : nullptr;
        UTexture2D* RoughnessSourceTexture = SourceTextureBySlot.Contains(TEXT("roughness")) ? FImageUtils::ImportFileAsTexture2D(SourceTextureBySlot[TEXT("roughness")]) : nullptr;
        UTexture2D* DisplacementSourceTexture = SourceTextureBySlot.Contains(TEXT("displacement")) ? FImageUtils::ImportFileAsTexture2D(SourceTextureBySlot[TEXT("displacement")]) : nullptr;
        UTexture2D* ORDPSourceTexture = SourceTextureBySlot.Contains(TEXT("ordp")) ? FImageUtils::ImportFileAsTexture2D(SourceTextureBySlot[TEXT("ordp")]) : nullptr;
        const FString AOPath = SourceTextureBySlot.Contains(TEXT("ao")) ? SourceTextureBySlot[TEXT("ao")] : TEXT("");
        const FString RoughnessPath = SourceTextureBySlot.Contains(TEXT("roughness")) ? SourceTextureBySlot[TEXT("roughness")] : TEXT("");
        const FString DisplacementPath = SourceTextureBySlot.Contains(TEXT("displacement")) ? SourceTextureBySlot[TEXT("displacement")] : TEXT("");
        const FString ORDPPath = SourceTextureBySlot.Contains(TEXT("ordp")) ? SourceTextureBySlot[TEXT("ordp")] : TEXT("");
        UE_LOG(LogTemp, Display, TEXT("AssetHive mask sources [%s] AO=%s Roughness=%s Displacement=%s ORDP=%s"), *AssetStem, *AOPath, *RoughnessPath, *DisplacementPath, *ORDPPath);
        UE_LOG(LogTemp, Display, TEXT("AssetHive mask source load [%s] AO=%d Roughness=%d Displacement=%d ORDP=%d"), *AssetStem, AOSourceTexture != nullptr ? 1 : 0, RoughnessSourceTexture != nullptr ? 1 : 0, DisplacementSourceTexture != nullptr ? 1 : 0, ORDPSourceTexture != nullptr ? 1 : 0);
        UTexture2D* AOTexture = AOSourceTexture ? AOSourceTexture : ORDPSourceTexture;
        UTexture2D* RoughnessTexture = RoughnessSourceTexture ? RoughnessSourceTexture : ORDPSourceTexture;
        UTexture2D* DisplacementTexture = DisplacementSourceTexture ? DisplacementSourceTexture : ORDPSourceTexture;
        const int32 AOChannel = AOSourceTexture ? 0 : (ORDPSourceTexture ? 0 : 0);
        const int32 RoughnessChannel = RoughnessSourceTexture ? 0 : (ORDPSourceTexture ? 1 : 0);
        const int32 DisplacementChannel = DisplacementSourceTexture ? 0 : (ORDPSourceTexture ? 2 : 0);
        UTexture2D* MaskTexture = CreatePackedMaskTexture(
            AssetFolder,
            AssetStem,
            AOTexture,
            AOChannel,
            RoughnessTexture,
            RoughnessChannel,
            DisplacementTexture,
            DisplacementChannel,
            Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("albedo"))),
            Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("normal")))
        );
        SetStageProgress(static_cast<float>(FMath::Clamp(AssetBaseProgress + 50, 0, 99)), FString::Printf(TEXT("创建材质实例: %s"), *AssetName));
        UMaterialInstanceConstant* MaterialInstance = CreateAssetMaterialInstance(
            AssetFolder,
            AssetStem,
            TextureBySlot.FindRef(TEXT("albedo")),
            TextureBySlot.FindRef(TEXT("normal")),
            MaskTexture,
            TextureBySlot.FindRef(TEXT("fuzz"))
        );
        if (MaterialInstance) {
            for (UStaticMesh* StaticMesh : ImportedMeshes) {
                if (!StaticMesh) {
                    continue;
                }
                const int32 SlotCount = FMath::Max(1, StaticMesh->GetStaticMaterials().Num());
                for (int32 Index = 0; Index < SlotCount; Index++) {
                    StaticMesh->SetMaterial(Index, MaterialInstance);
                }
                StaticMesh->PostEditChange();
                StaticMesh->MarkPackageDirty();
                SavePackageForObject(StaticMesh);
            }
        }
        WriteImportSignal(AssetFolder);
        SetStageProgress(static_cast<float>(AssetEndProgress), FString::Printf(TEXT("资产完成: %s"), *AssetName));
        AssetIndex++;
    }
    SetStageProgress(100.0f, TEXT("导入完成"));
    UE_LOG(LogTemp, Display, TEXT("AssetHive import completed: %s"), *JobFilePath);
    return 0;
}
