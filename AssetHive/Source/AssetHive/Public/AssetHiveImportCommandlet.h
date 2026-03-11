#pragma once

#include "Commandlets/Commandlet.h"
#include "AssetHiveImportCommandlet.generated.h"

UCLASS()
class ASSETHIVE_API UAssetHiveImportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UAssetHiveImportCommandlet();
    virtual int32 Main(const FString& Params) override;
};
