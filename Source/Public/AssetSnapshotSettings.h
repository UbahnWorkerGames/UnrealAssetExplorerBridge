#pragma once

#include "Engine/DeveloperSettings.h"
#include "AssetSnapshotImportTypes.h"
#include "AssetSnapshotSettings.generated.h"

UCLASS(config=Engine, defaultconfig, meta=(DisplayName="Asset Meta Explorer Bridge"))
class ASSETMETAEXPLORERBRIDGE_API UAssetSnapshotSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    virtual FName GetCategoryName() const override { return FName("Plugins"); }
    virtual FName GetSectionName() const override { return FName("Asset Meta Explorer Bridge"); }

    UPROPERTY(EditAnywhere, Config, Category="Import")
    FString ImportBaseUrl = TEXT("127.0.0.1:9090");

    UPROPERTY(EditAnywhere, Config, Category="Import", meta=(ClampMin="1", ClampMax="65535"))
    int32 ImportListenPort = 8008;
};
