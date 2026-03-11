#pragma once

#include "Modules/ModuleInterface.h"
#include "Templates/SharedPointer.h"
#include "Containers/Ticker.h"

class FSlateStyleSet;

class FAssetHiveModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    bool TickConnection(float DeltaTime);
    void UpdateConnectionState();
    void SetupStyle();
    void TeardownStyle();
    void ConsumeImportSignal();

    bool bConnectedToAssetHive = false;
    int64 LastImportSignalTimestamp = 0;
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<FSlateStyleSet> StyleSet;
};
