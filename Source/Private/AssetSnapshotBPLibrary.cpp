#include "AssetSnapshotBPLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture.h"
#include "CanvasTypes.h"
#include "CanvasItem.h"
#include "RHI.h"
#include "webp/encode.h"
// UE5: IStreamingManager lives in ContentStreaming.h (the old Streaming/StreamingManager.h path no longer exists)
#include "ContentStreaming.h"
#include "PhysicsEngine/BodySetup.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Modules/ModuleManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/ThreadSafeBool.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "PreviewScene.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "AssetSnapshotSettings.h"

#if WITH_EDITOR
#include "Editor.h"
#include "AssetCompilingManager.h"
#endif

// Optional types
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Engine/Blueprint.h"

#if __has_include("NiagaraSystem.h")
#include "NiagaraSystem.h"
#include "NiagaraComponent.h"
#define ASSETSNAPSHOT_WITH_NIAGARA 1
#else
#define ASSETSNAPSHOT_WITH_NIAGARA 0
#endif

#include "blake3.h"

DEFINE_LOG_CATEGORY_STATIC(LogAssetSnapshot, Log, All);

static int32 GAssetSnapshotExportBatchId = 0;
static int32 GAssetSnapshotServerBatchId = -1;
static int32 GAssetSnapshotExportTotal = 0;
static int32 GAssetSnapshotExportCurrent = 0;
static bool GAssetSnapshotServerChecked = false;
static bool GAssetSnapshotServerAvailable = true;
static bool GAssetSnapshotServerWarned = false;
static bool GAssetSnapshotServerSkipKnown = false;
static bool GAssetSnapshotServerSkipEnabled = true;

namespace AssetSnapshot
{
    static FString NormalizeBaseUrl(const FString& InBaseUrl)
    {
        FString Url = InBaseUrl;
        Url.TrimStartAndEndInline();
        if (!Url.StartsWith(TEXT("http://")) && !Url.StartsWith(TEXT("https://")))
        {
            Url = TEXT("http://") + Url;
        }
        while (Url.EndsWith(TEXT("/")))
        {
            Url.LeftChopInline(1);
        }
        return Url;
    }


    struct FServerSettingsCache
    {
        bool bFetched = false;
        bool bAvailable = false;
    double LastFetchTimeSec = 0.0;
        FString BaseUrl;
        bool bOverwriteExportZips = false;
        int32 DefaultImageCount = 1;
        int32 StaticMeshImageCount = 0;
        int32 SkeletalMeshImageCount = 0;
        int32 MaterialImageCount = 0;
        int32 BlueprintImageCount = 0;
        int32 NiagaraImageCount = 0;
        int32 AnimSequenceImageCount = 0;
        int32 Capture360DiscardFrames = 2;
        bool bSkipExportIfOnServer = false;
        FString ExportCheckPathTemplate = TEXT("/assets/exists?hash={hash}&hash_type=blake3");
        bool bUploadAfterExport = true;
        FString ExportUploadPathTemplate = TEXT("/assets/upload");
    };

    static FServerSettingsCache GServerSettings;
    static FCriticalSection GServerSettingsLock;

    static bool ParseBoolSetting(const FString& Value, bool DefaultValue)
    {
        FString Raw = Value;
        Raw.TrimStartAndEndInline();
        Raw = Raw.ToLower();
        if (Raw.IsEmpty())
        {
            return DefaultValue;
        }
        return Raw == TEXT("1") || Raw == TEXT("true") || Raw == TEXT("yes") || Raw == TEXT("on");
    }

    static int32 ParseIntSetting(const FString& Value, int32 DefaultValue)
    {
        FString Raw = Value;
        Raw.TrimStartAndEndInline();
        if (Raw.IsEmpty())
        {
            return DefaultValue;
        }
        if (!Raw.IsNumeric())
        {
            return DefaultValue;
        }
        return FCString::Atoi(*Raw);
    }

    static FString GetSettingString(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const FString& DefaultValue)
    {
        FString Out;
        if (Obj.IsValid())
        {
            if (Obj->TryGetStringField(Key, Out))
            {
                return Out;
            }
            bool bBool = false;
            if (Obj->TryGetBoolField(Key, bBool))
            {
                return bBool ? TEXT("true") : TEXT("false");
            }
            double Num = 0.0;
            if (Obj->TryGetNumberField(Key, Num))
            {
                return FString::Printf(TEXT("%d"), (int32)Num);
            }
        }
        return DefaultValue;
    }

    static const FServerSettingsCache& GetServerSettingsCached(const FString& BaseUrl)
    {
        FScopeLock Lock(&GServerSettingsLock);
        const double NowSec = FPlatformTime::Seconds();
        if (GServerSettings.bFetched && GServerSettings.BaseUrl == BaseUrl)
        {
            if ((NowSec - GServerSettings.LastFetchTimeSec) < 10.0)
            {
                return GServerSettings;
            }
        }

        GServerSettings = FServerSettingsCache();
        GServerSettings.BaseUrl = BaseUrl;
        GServerSettings.bFetched = true;
        GServerSettings.bAvailable = false;
        GServerSettings.LastFetchTimeSec = NowSec;

        if (BaseUrl.IsEmpty())
        {
            return GServerSettings;
        }

        const FString Url = NormalizeBaseUrl(BaseUrl) + TEXT("/settings");
        TSharedPtr<FEvent> DoneEvent = MakeShareable(
            FPlatformProcess::GetSynchEventFromPool(true),
            [](FEvent* E) { FPlatformProcess::ReturnSynchEventToPool(E); });
        TSharedRef<TAtomic<bool>> bDone = MakeShared<TAtomic<bool>>(false);
        TSharedRef<TAtomic<bool>> bOk = MakeShared<TAtomic<bool>>(false);
        TSharedRef<TAtomic<bool>> bAbandoned = MakeShared<TAtomic<bool>>(false);
        TSharedRef<FString> ResponseText = MakeShared<FString>();

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(Url);
        Request->SetVerb(TEXT("GET"));
        Request->OnProcessRequestComplete().BindLambda(
            [bDone, bOk, bAbandoned, DoneEvent, ResponseText](TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Req, TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Resp, bool bSucceeded)
            {
                if (bAbandoned->Load())
                {
                    return;
                }
                if (bSucceeded && Resp.IsValid() && EHttpResponseCodes::IsOk(Resp->GetResponseCode()))
                {
                    *ResponseText = Resp->GetContentAsString();
                    bOk->Store(true);
                }
                bDone->Store(true);
                DoneEvent->Trigger();
            });
        Request->ProcessRequest();

        const double WaitStart = FPlatformTime::Seconds();
        while (!bDone->Load() && (FPlatformTime::Seconds() - WaitStart) < 5.0)
        {
            FHttpModule::Get().GetHttpManager().Tick(0.01f);
            FPlatformProcess::Sleep(0.01f);
        }

        if (!bDone->Load())
        {
            bAbandoned->Store(true);
            Request->CancelRequest();
            UE_LOG(LogAssetSnapshot, Warning, TEXT("Server settings request timed out: %s"), *Url);
            return GServerSettings;
        }
        if (!bOk->Load())
        {
            return GServerSettings;
        }

        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseText);
        if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
        {
            return GServerSettings;
        }

        const FString DefaultCount = GetSettingString(Obj, TEXT("export_default_image_count"), TEXT("1"));
        const int32 DefaultCountInt = ParseIntSetting(DefaultCount, 1);

        GServerSettings.bOverwriteExportZips = ParseBoolSetting(GetSettingString(Obj, TEXT("export_overwrite_zips"), TEXT("false")), false);
        GServerSettings.DefaultImageCount = DefaultCountInt;
        GServerSettings.StaticMeshImageCount = ParseIntSetting(GetSettingString(Obj, TEXT("export_static_mesh_image_count"), TEXT("")), 0);
        GServerSettings.SkeletalMeshImageCount = ParseIntSetting(GetSettingString(Obj, TEXT("export_skeletal_mesh_image_count"), TEXT("")), 0);
        GServerSettings.MaterialImageCount = ParseIntSetting(GetSettingString(Obj, TEXT("export_material_image_count"), TEXT("")), 0);
        GServerSettings.BlueprintImageCount = ParseIntSetting(GetSettingString(Obj, TEXT("export_blueprint_image_count"), TEXT("")), 0);
        GServerSettings.NiagaraImageCount = ParseIntSetting(GetSettingString(Obj, TEXT("export_niagara_image_count"), TEXT("")), 0);
        GServerSettings.AnimSequenceImageCount = ParseIntSetting(GetSettingString(Obj, TEXT("export_anim_sequence_image_count"), TEXT("")), 0);
        GServerSettings.Capture360DiscardFrames = ParseIntSetting(GetSettingString(Obj, TEXT("export_capture360_discard_frames"), TEXT("0")), 0);
        GServerSettings.bSkipExportIfOnServer = ParseBoolSetting(GetSettingString(Obj, TEXT("skip_export_if_on_server"), TEXT("false")), false);
        GServerSettings.ExportCheckPathTemplate = GetSettingString(Obj, TEXT("export_check_path_template"), TEXT("/assets/exists?hash={hash}&hash_type=blake3"));
        GServerSettings.bUploadAfterExport = ParseBoolSetting(GetSettingString(Obj, TEXT("export_upload_after_export"), TEXT("true")), true);
        GServerSettings.ExportUploadPathTemplate = GetSettingString(Obj, TEXT("export_upload_path_template"), TEXT("/assets/upload"));
        GServerSettings.bAvailable = true;
        GServerSettings.LastFetchTimeSec = NowSec;
        return GServerSettings;
    }

    static const int32 kDefaultResolution = 1024;
    static const float kDefaultFov = 30.0f;
    static const float kDistancePadding = 1.15f;
    
    // ============================================================================
    // SINGLE EXPORT WITH 1 SECOND PAUSE BEFORE SHOOTING
    // ============================================================================
    // For 360° View (StaticMesh, SkeletalMesh):
    static const int32 kCapture360FramesToDiscardDefault = 0;
    static int32 GetCapture360DiscardCount()
    {
        const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings ? Settings->ImportBaseUrl : FString());
        return FMath::Clamp(Server.Capture360DiscardFrames, 0, 10);
    }

    static int32 ClampCount(int32 Value, int32 DefaultValue)
    {
        const int32 Raw = Value > 0 ? Value : DefaultValue;
        return FMath::Clamp(Raw, 1, 24);
    }

    static int32 GetStaticMeshFrameCount()
    {
        const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings ? Settings->ImportBaseUrl : FString());
        const int32 DefaultCount = Server.DefaultImageCount > 0 ? Server.DefaultImageCount : 1;
        const int32 Value = Server.StaticMeshImageCount > 0 ? Server.StaticMeshImageCount : DefaultCount;
        return ClampCount(Value, DefaultCount);
    }

    static int32 GetSkeletalMeshFrameCount()
    {
        const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings ? Settings->ImportBaseUrl : FString());
        const int32 DefaultCount = Server.DefaultImageCount > 0 ? Server.DefaultImageCount : 1;
        const int32 Value = Server.SkeletalMeshImageCount > 0 ? Server.SkeletalMeshImageCount : DefaultCount;
        return ClampCount(Value, DefaultCount);
    }

    static int32 GetBlueprintFrameCount()
    {
        const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings ? Settings->ImportBaseUrl : FString());
        const int32 DefaultCount = Server.DefaultImageCount > 0 ? Server.DefaultImageCount : 1;
        const int32 Value = Server.BlueprintImageCount > 0 ? Server.BlueprintImageCount : DefaultCount;
        return ClampCount(Value, DefaultCount);
    }

    static int32 GetMaterialFrameCount()
    {
        const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings ? Settings->ImportBaseUrl : FString());
        const int32 DefaultCount = Server.DefaultImageCount > 0 ? Server.DefaultImageCount : 1;
        const int32 Value = Server.MaterialImageCount > 0 ? Server.MaterialImageCount : DefaultCount;
        return ClampCount(Value, DefaultCount);
    }

    static int32 GetAnimFrameCount()
    {
        const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings ? Settings->ImportBaseUrl : FString());
        const int32 DefaultCount = Server.DefaultImageCount > 0 ? Server.DefaultImageCount : 1;
        const int32 Value = Server.AnimSequenceImageCount > 0 ? Server.AnimSequenceImageCount : 4;
        return ClampCount(Value, DefaultCount);
    }
    static const int32 kCapture360FramesTotal = 8;          // Total frames
    static const int32 kCapture360FramesToKeep = 5;         // Keep 5 frames (72 deg per frame)
    static const float kCapture360FrameInterval = 0.2f;     // 0.2s between frames
    static const float kCapture360PauseBeforeShoot = 1.0f; // 1 SECOND PAUSE before shooting!
    // Strategy: 1s pause, then capture 5 frames (keep 5)
    // Total: ~3.5 seconds (1s pause + 2.5s capture)
    
    // For Materials (animated materials, NO 360° rotation):
    static const int32 kCaptureMaterialFramesTotal = 5;     // Total frames
    static const int32 kCaptureMaterialFramesToKeep = 5;    // Keep 5 frames (animation)
    static const float kCaptureMaterialFrameInterval = 0.5f; // 0.5s between frames
    static const float kCaptureMaterialPauseBeforeShoot = 1.0f; // 1 SECOND PAUSE before shooting!
    // Strategy: 1s pause, then capture 5 frames (keep 5)
    // Total: ~3.5 seconds (1s pause + 2.5s capture)
    // ============================================================================
    
    static const int32 kMaxAnimationFrames = 10;
    static const int32 kWarmupFrames = 60;
    static const float kWarmupSeconds = 8.0f;
    static const float kWarmupPauseSeconds = 6.0f;
    static const int32 kMinMaterialResolution = 1024;
    static const int32 kTexturePreviewResolution = 1024;
    static const int32 kMaterialMinWebPBytes = 130000;

    struct FZipEntry
    {
        FString NameInZip;
        TArray<uint8> Data;
    };

    struct FMaterialCaptureContext
    {
        FPreviewScene Scene;
        UWorld* World = nullptr;
        UStaticMeshComponent* Comp = nullptr;
        FVector ViewDir = FVector(1.f, 0.f, 0.f);
        float Distance = 0.f;

        FMaterialCaptureContext()
            : Scene(FPreviewScene::ConstructionValues())
        {
        }
    };

    static FMaterialCaptureContext* GMaterialCaptureContext = nullptr;

    static void WriteLE16(FArchive& Ar, uint16 V) { Ar.Serialize(&V, sizeof(V)); }
    static void WriteLE32(FArchive& Ar, uint32 V) { Ar.Serialize(&V, sizeof(V)); }
    static bool ReadLE16(FArchive& Ar, uint16& Out)
    {
        Ar.Serialize(&Out, sizeof(Out));
        return !Ar.IsError();
    }
    static bool ReadLE32(FArchive& Ar, uint32& Out)
    {
        Ar.Serialize(&Out, sizeof(Out));
        return !Ar.IsError();
    }

    struct FCentralDirEntry
    {
        FString Name;
        uint32 Crc32 = 0;
        uint32 CompSize = 0;
        uint32 UncompSize = 0;
        uint32 LocalHeaderOffset = 0;
    };

    // Creates a simple "store" ZIP (no compression). Good enough for backend import.
    static bool WriteZipStore(const FString& ZipPath, const TArray<FZipEntry>& Entries)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(ZipPath), true);
        TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*ZipPath));
        if (!Ar)
        {
            UE_LOG(LogAssetSnapshot, Error, TEXT("Failed to create zip: %s"), *ZipPath);
            return false;
        }

        TArray<FCentralDirEntry> Central;
        Central.Reserve(Entries.Num());

        for (const FZipEntry& E : Entries)
        {
            FTCHARToUTF8 NameUtf8(*E.NameInZip);
            const uint16 NameLen = (uint16)NameUtf8.Length();

            FCentralDirEntry C;
            C.Name = E.NameInZip;
            C.UncompSize = (uint32)E.Data.Num();
            C.CompSize = C.UncompSize;
            C.Crc32 = (uint32)FCrc::MemCrc32(E.Data.GetData(), E.Data.Num());
            C.LocalHeaderOffset = (uint32)Ar->Tell();

            // Local file header
            WriteLE32(*Ar, 0x04034b50);
            WriteLE16(*Ar, 20); // version needed
            WriteLE16(*Ar, 0);  // flags
            WriteLE16(*Ar, 0);  // method 0 = store
            WriteLE16(*Ar, 0);  // mod time
            WriteLE16(*Ar, 0);  // mod date
            WriteLE32(*Ar, C.Crc32);
            WriteLE32(*Ar, C.CompSize);
            WriteLE32(*Ar, C.UncompSize);
            WriteLE16(*Ar, NameLen);
            WriteLE16(*Ar, 0); // extra len
            Ar->Serialize((void*)NameUtf8.Get(), NameLen);
            Ar->Serialize((void*)E.Data.GetData(), E.Data.Num());

            Central.Add(C);
        }

        const uint32 CentralDirOffset = (uint32)Ar->Tell();

        // Central directory
        for (const FCentralDirEntry& C : Central)
        {
            FTCHARToUTF8 NameUtf8(*C.Name);
            const uint16 NameLen = (uint16)NameUtf8.Length();

            WriteLE32(*Ar, 0x02014b50);
            WriteLE16(*Ar, 20); // version made by
            WriteLE16(*Ar, 20); // version needed
            WriteLE16(*Ar, 0);  // flags
            WriteLE16(*Ar, 0);  // method
            WriteLE16(*Ar, 0);  // time
            WriteLE16(*Ar, 0);  // date
            WriteLE32(*Ar, C.Crc32);
            WriteLE32(*Ar, C.CompSize);
            WriteLE32(*Ar, C.UncompSize);
            WriteLE16(*Ar, NameLen);
            WriteLE16(*Ar, 0); // extra
            WriteLE16(*Ar, 0); // comment
            WriteLE16(*Ar, 0); // disk
            WriteLE16(*Ar, 0); // internal attrs
            WriteLE32(*Ar, 0); // external attrs
            WriteLE32(*Ar, C.LocalHeaderOffset);
            Ar->Serialize((void*)NameUtf8.Get(), NameLen);
        }

        const uint32 CentralDirSize = (uint32)Ar->Tell() - CentralDirOffset;

        // End of central directory
        WriteLE32(*Ar, 0x06054b50);
        WriteLE16(*Ar, 0);
        WriteLE16(*Ar, 0);
        WriteLE16(*Ar, (uint16)Central.Num());
        WriteLE16(*Ar, (uint16)Central.Num());
        WriteLE32(*Ar, CentralDirSize);
        WriteLE32(*Ar, CentralDirOffset);
        WriteLE16(*Ar, 0);

        Ar->Close();
        return true;
    }

    static FString ToLowerHex(const uint8* Bytes, int32 NumBytes)
    {
        static const TCHAR* Hex = TEXT("0123456789abcdef");
        FString Out;
        Out.Reserve(NumBytes * 2);
        for (int32 i = 0; i < NumBytes; ++i)
        {
            const uint8 B = Bytes[i];
            Out.AppendChar(Hex[(B >> 4) & 0xF]);
            Out.AppendChar(Hex[B & 0xF]);
        }
        return Out;
    }

    static bool Blake3HashFile(const FString& FileAbs, FString& OutHex)
    {
        TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*FileAbs));
        if (!Ar)
        {
            return false;
        }

        blake3_hasher Hasher;
        blake3_hasher_init(&Hasher);

        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(1024 * 1024);

        while (!Ar->AtEnd())
        {
            const int64 Remaining = Ar->TotalSize() - Ar->Tell();
            const int64 ToRead = FMath::Min<int64>(Remaining, Buffer.Num());
            Ar->Serialize(Buffer.GetData(), ToRead);
            blake3_hasher_update(&Hasher, Buffer.GetData(), (size_t)ToRead);
        }

        uint8 Out[32];
        blake3_hasher_finalize(&Hasher, Out, sizeof(Out));
        OutHex = ToLowerHex(Out, sizeof(Out));
        return true;
    }

    static bool Sha256HashFile(const FString& FileAbs, FString& OutHex)
    {
        struct FSha256Ctx
        {
            uint32 State[8];
            uint64 BitLen = 0;
            uint8 Data[64];
            uint32 DataLen = 0;
        };

        static const uint32 K[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };

        auto RotR = [](uint32 X, uint32 N) { return (X >> N) | (X << (32 - N)); };
        auto Ch = [](uint32 X, uint32 Y, uint32 Z) { return (X & Y) ^ (~X & Z); };
        auto Maj = [](uint32 X, uint32 Y, uint32 Z) { return (X & Y) ^ (X & Z) ^ (Y & Z); };
        auto Sig0 = [&](uint32 X) { return RotR(X, 2) ^ RotR(X, 13) ^ RotR(X, 22); };
        auto Sig1 = [&](uint32 X) { return RotR(X, 6) ^ RotR(X, 11) ^ RotR(X, 25); };
        auto Theta0 = [&](uint32 X) { return RotR(X, 7) ^ RotR(X, 18) ^ (X >> 3); };
        auto Theta1 = [&](uint32 X) { return RotR(X, 17) ^ RotR(X, 19) ^ (X >> 10); };

        auto Transform = [&](FSha256Ctx& Ctx, const uint8 Block[64])
        {
            uint32 W[64];
            for (int32 i = 0; i < 16; ++i)
            {
                W[i] = (uint32)Block[i * 4 + 0] << 24 |
                       (uint32)Block[i * 4 + 1] << 16 |
                       (uint32)Block[i * 4 + 2] << 8  |
                       (uint32)Block[i * 4 + 3];
            }
            for (int32 i = 16; i < 64; ++i)
            {
                W[i] = Theta1(W[i - 2]) + W[i - 7] + Theta0(W[i - 15]) + W[i - 16];
            }

            uint32 A = Ctx.State[0];
            uint32 B = Ctx.State[1];
            uint32 C = Ctx.State[2];
            uint32 D = Ctx.State[3];
            uint32 E = Ctx.State[4];
            uint32 F = Ctx.State[5];
            uint32 G = Ctx.State[6];
            uint32 H = Ctx.State[7];

            for (int32 i = 0; i < 64; ++i)
            {
                const uint32 T1 = H + Sig1(E) + Ch(E, F, G) + K[i] + W[i];
                const uint32 T2 = Sig0(A) + Maj(A, B, C);
                H = G;
                G = F;
                F = E;
                E = D + T1;
                D = C;
                C = B;
                B = A;
                A = T1 + T2;
            }

            Ctx.State[0] += A;
            Ctx.State[1] += B;
            Ctx.State[2] += C;
            Ctx.State[3] += D;
            Ctx.State[4] += E;
            Ctx.State[5] += F;
            Ctx.State[6] += G;
            Ctx.State[7] += H;
        };

        auto Init = [&](FSha256Ctx& Ctx)
        {
            Ctx.State[0] = 0x6a09e667u;
            Ctx.State[1] = 0xbb67ae85u;
            Ctx.State[2] = 0x3c6ef372u;
            Ctx.State[3] = 0xa54ff53au;
            Ctx.State[4] = 0x510e527fu;
            Ctx.State[5] = 0x9b05688cu;
            Ctx.State[6] = 0x1f83d9abu;
            Ctx.State[7] = 0x5be0cd19u;
            Ctx.BitLen = 0;
            Ctx.DataLen = 0;
        };

        auto Update = [&](FSha256Ctx& Ctx, const uint8* Data, uint32 Len)
        {
            for (uint32 i = 0; i < Len; ++i)
            {
                Ctx.Data[Ctx.DataLen++] = Data[i];
                if (Ctx.DataLen == 64)
                {
                    Transform(Ctx, Ctx.Data);
                    Ctx.BitLen += 512;
                    Ctx.DataLen = 0;
                }
            }
        };

        auto Final = [&](FSha256Ctx& Ctx, uint8 Hash[32])
        {
            uint32 i = Ctx.DataLen;

            // Pad
            if (Ctx.DataLen < 56)
            {
                Ctx.Data[i++] = 0x80;
                while (i < 56)
                {
                    Ctx.Data[i++] = 0x00;
                }
            }
            else
            {
                Ctx.Data[i++] = 0x80;
                while (i < 64)
                {
                    Ctx.Data[i++] = 0x00;
                }
                Transform(Ctx, Ctx.Data);
                FMemory::Memset(Ctx.Data, 0, 56);
            }

            Ctx.BitLen += Ctx.DataLen * 8;
            Ctx.Data[63] = (uint8)(Ctx.BitLen);
            Ctx.Data[62] = (uint8)(Ctx.BitLen >> 8);
            Ctx.Data[61] = (uint8)(Ctx.BitLen >> 16);
            Ctx.Data[60] = (uint8)(Ctx.BitLen >> 24);
            Ctx.Data[59] = (uint8)(Ctx.BitLen >> 32);
            Ctx.Data[58] = (uint8)(Ctx.BitLen >> 40);
            Ctx.Data[57] = (uint8)(Ctx.BitLen >> 48);
            Ctx.Data[56] = (uint8)(Ctx.BitLen >> 56);
            Transform(Ctx, Ctx.Data);

            for (int32 j = 0; j < 4; ++j)
            {
                Hash[j]      = (uint8)((Ctx.State[0] >> (24 - j * 8)) & 0xff);
                Hash[j + 4]  = (uint8)((Ctx.State[1] >> (24 - j * 8)) & 0xff);
                Hash[j + 8]  = (uint8)((Ctx.State[2] >> (24 - j * 8)) & 0xff);
                Hash[j + 12] = (uint8)((Ctx.State[3] >> (24 - j * 8)) & 0xff);
                Hash[j + 16] = (uint8)((Ctx.State[4] >> (24 - j * 8)) & 0xff);
                Hash[j + 20] = (uint8)((Ctx.State[5] >> (24 - j * 8)) & 0xff);
                Hash[j + 24] = (uint8)((Ctx.State[6] >> (24 - j * 8)) & 0xff);
                Hash[j + 28] = (uint8)((Ctx.State[7] >> (24 - j * 8)) & 0xff);
            }
        };

        TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*FileAbs));
        if (!Ar)
        {
            return false;
        }

        FSha256Ctx Ctx;
        Init(Ctx);

        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(1024 * 1024);

        while (!Ar->AtEnd())
        {
            const int64 Remaining = Ar->TotalSize() - Ar->Tell();
            const int64 ToRead = FMath::Min<int64>(Remaining, Buffer.Num());
            Ar->Serialize(Buffer.GetData(), ToRead);
            Update(Ctx, Buffer.GetData(), (uint32)ToRead);
        }

        uint8 Out[32];
        Final(Ctx, Out);
        OutHex = ToLowerHex(Out, sizeof(Out));
        return true;
    }

    static bool Blake3HashFiles(const TArray<FString>& FilesAbsSorted, const TArray<FString>& FilesRelSorted, FString& OutHex)
    {
        if (FilesAbsSorted.Num() != FilesRelSorted.Num())
        {
            return false;
        }

        blake3_hasher Hasher;
        blake3_hasher_init(&Hasher);

        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(1024 * 1024);

        for (int32 i = 0; i < FilesAbsSorted.Num(); ++i)
        {
            const FString& Abs = FilesAbsSorted[i];
            const FString& Rel = FilesRelSorted[i];

            FTCHARToUTF8 RelUtf8(*Rel);
            blake3_hasher_update(&Hasher, RelUtf8.Get(), (size_t)RelUtf8.Length());
            const uint8 Zero = 0;
            blake3_hasher_update(&Hasher, &Zero, 1);

            TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Abs));
            if (!Ar)
            {
                // If a file disappears, we still produce a deterministic hash based on path only
                continue;
            }

            while (!Ar->AtEnd())
            {
                const int64 Remaining = Ar->TotalSize() - Ar->Tell();
                const int64 ToRead = FMath::Min<int64>(Remaining, Buffer.Num());
                Ar->Serialize(Buffer.GetData(), ToRead);
                blake3_hasher_update(&Hasher, Buffer.GetData(), (size_t)ToRead);
            }
        }

        uint8 Out[32];
        blake3_hasher_finalize(&Hasher, Out, sizeof(Out));
        OutHex = ToLowerHex(Out, sizeof(Out));
        return true;
    }

    static bool EncodeWebPFromBGRA(const TArray<FColor>& Pixels, int32 Width, int32 Height, TArray<uint8>& OutBytes)
    {
        if (Pixels.Num() == 0 || Width <= 0 || Height <= 0)
        {
            return false;
        }

        const uint8* Raw = reinterpret_cast<const uint8*>(Pixels.GetData());
        const int32 Stride = Width * 4;
        uint8* Encoded = nullptr;
        const size_t EncodedSize = WebPEncodeBGRA(Raw, Width, Height, Stride, 80.0f, &Encoded);
        if (EncodedSize == 0 || Encoded == nullptr)
        {
            return false;
        }

        OutBytes.SetNumUninitialized((int32)EncodedSize);
        FMemory::Memcpy(OutBytes.GetData(), Encoded, EncodedSize);
        WebPFree(Encoded);
        return true;
    }

    static bool MakeBlackWebP(int32 Size, TArray<uint8>& OutWebP)
    {
        if (Size <= 0)
        {
            return false;
        }

        TArray<FColor> Pixels;
        Pixels.Init(FColor::Black, Size * Size);
        return EncodeWebPFromBGRA(Pixels, Size, Size, OutWebP);
    }

    static void AddBlackPreview(
        TArray<TSharedPtr<FJsonValue>>& PreviewFiles,
        TArray<FZipEntry>& ZipEntries,
        int32 Size)
    {
        TArray<uint8> WebP;
        if (!MakeBlackWebP(Size, WebP))
        {
            return;
        }

        FZipEntry E;
        E.NameInZip = TEXT("0.webp");
        E.Data = MoveTemp(WebP);
        PreviewFiles.Add(MakeShared<FJsonValueString>(E.NameInZip));
        ZipEntries.Add(MoveTemp(E));
    }

    static FString NormalizeRelPath(const FString& Path)
    {
        FString P = Path;
        P.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (P.StartsWith(TEXT("/")))
        {
            P.RightChopInline(1);
        }
        return P;
    }

    // Project-layout zips can contain "<Project>/Content/...".
    // We keep the project folder, but drop the intermediate "Content" segment
    // so imports become "<Project>/..." under the target Content root.
    static FString NormalizeImportRelPath(const FString& RelPath)
    {
        FString P = NormalizeRelPath(RelPath);
        if (P.IsEmpty())
        {
            return P;
        }

        TArray<FString> Parts;
        P.ParseIntoArray(Parts, TEXT("/"), true);
        if (Parts.Num() >= 2 && Parts[1].Equals(TEXT("Content"), ESearchCase::IgnoreCase))
        {
            Parts.RemoveAt(1);
            return FString::Join(Parts, TEXT("/"));
        }
        if (Parts.Num() >= 1 && Parts[0].Equals(TEXT("Content"), ESearchCase::IgnoreCase))
        {
            Parts.RemoveAt(0);
            return FString::Join(Parts, TEXT("/"));
        }
        return P;
    }

    static bool IsSafeZipRelPath(const FString& RelPath)
    {
        FString P = NormalizeRelPath(RelPath);
        if (P.IsEmpty())
        {
            return false;
        }
        if (P.Contains(TEXT(":")))
        {
            return false;
        }
        TArray<FString> Parts;
        P.ParseIntoArray(Parts, TEXT("/"), true);
        for (const FString& Part : Parts)
        {
            if (Part == TEXT("..") || Part == TEXT("."))
            {
                return false;
            }
        }
        return true;
    }

    static bool IsImportableAssetFile(const FString& RelPath)
    {
        const FString Ext = FString::Printf(TEXT(".%s"), *FPaths::GetExtension(RelPath, false)).ToLower();
        return Ext == TEXT(".uasset")
            || Ext == TEXT(".uexp")
            || Ext == TEXT(".ubulk")
            || Ext == TEXT(".uptnl")
            || Ext == TEXT(".umap");
    }

    static bool ExtractZipStore(const FString& ZipPath, const FString& DestRoot, EAssetSnapshotImportMode Mode, FString& OutError)
    {
        TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*ZipPath));
        if (!Ar)
        {
            OutError = FString::Printf(TEXT("Failed to open zip: %s"), *ZipPath);
            return false;
        }

        int32 ImportedFiles = 0;
        int32 SkippedFiles = 0;
        TArray<FString> ImportedUAssetFiles;

        while (!Ar->AtEnd())
        {
            uint32 Sig = 0;
            if (!ReadLE32(*Ar, Sig))
            {
                OutError = TEXT("Failed to read zip signature.");
                return false;
            }

            if (Sig == 0x02014b50 || Sig == 0x06054b50)
            {
                // Central directory or end of central directory; we're done.
                break;
            }

            if (Sig != 0x04034b50)
            {
                OutError = FString::Printf(TEXT("Unexpected zip signature: 0x%08x"), Sig);
                return false;
            }

            uint16 Version = 0;
            uint16 Flags = 0;
            uint16 Method = 0;
            uint16 ModTime = 0;
            uint16 ModDate = 0;
            uint32 Crc32 = 0;
            uint32 CompSize = 0;
            uint32 UncompSize = 0;
            uint16 NameLen = 0;
            uint16 ExtraLen = 0;

            if (!ReadLE16(*Ar, Version) ||
                !ReadLE16(*Ar, Flags) ||
                !ReadLE16(*Ar, Method) ||
                !ReadLE16(*Ar, ModTime) ||
                !ReadLE16(*Ar, ModDate) ||
                !ReadLE32(*Ar, Crc32) ||
                !ReadLE32(*Ar, CompSize) ||
                !ReadLE32(*Ar, UncompSize) ||
                !ReadLE16(*Ar, NameLen) ||
                !ReadLE16(*Ar, ExtraLen))
            {
                OutError = TEXT("Failed to read zip local header.");
                return false;
            }

            TArray<uint8> NameBytes;
            NameBytes.SetNumUninitialized(NameLen);
            Ar->Serialize(NameBytes.GetData(), NameLen);

            FString Name;
            if (NameLen > 0)
            {
                FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(NameBytes.GetData()), NameBytes.Num());
                Name = FString(Conv.Length(), Conv.Get());
            }

            if (ExtraLen > 0)
            {
                Ar->Seek(Ar->Tell() + ExtraLen);
            }

            if (Method != 0)
            {
                OutError = FString::Printf(TEXT("Unsupported zip compression method: %d"), Method);
                return false;
            }

            TArray<uint8> Data;
            Data.SetNumUninitialized(CompSize);
            if (CompSize > 0)
            {
                Ar->Serialize(Data.GetData(), CompSize);
            }

            if (Name.IsEmpty() || Name.EndsWith(TEXT("/")))
            {
                continue;
            }

            if (!IsSafeZipRelPath(Name))
            {
                OutError = FString::Printf(TEXT("Unsafe zip path: %s"), *Name);
                return false;
            }

            const FString RelPath = NormalizeImportRelPath(Name);
            const FString DestPath = FPaths::ConvertRelativePathToFull(DestRoot / RelPath);

            if (!IsImportableAssetFile(RelPath))
            {
                ++SkippedFiles;
                continue;
            }

            if (Mode == EAssetSnapshotImportMode::SkipExisting && IFileManager::Get().FileExists(*DestPath))
            {
                ++SkippedFiles;
                continue;
            }

            IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestPath), true);
            if (!FFileHelper::SaveArrayToFile(Data, *DestPath))
            {
                OutError = FString::Printf(TEXT("Failed to write file: %s"), *DestPath);
                return false;
            }

            if (DestPath.EndsWith(TEXT(".uasset")) || DestPath.EndsWith(TEXT(".umap")))
            {
                ImportedUAssetFiles.Add(DestPath);
            }
            ++ImportedFiles;
        }

        if (ImportedUAssetFiles.Num() > 0)
        {
            FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
            ARM.Get().ScanFilesSynchronous(ImportedUAssetFiles, true);
        }

        UE_LOG(LogAssetSnapshot, Log, TEXT("Import complete. Imported: %d, Skipped: %d"), ImportedFiles, SkippedFiles);
        return true;
    }


    static FString BuildSnapshotUrl(const FString& BaseUrl, const FString& PathTemplate, const FString& SnapshotId)
    {
        FString Url = NormalizeBaseUrl(BaseUrl);

        FString Path = PathTemplate;
        Path.TrimStartAndEndInline();
        if (Path.IsEmpty())
        {
            Path = TEXT("/download/{id}.zip");
        }
        if (!Path.StartsWith(TEXT("/")))
        {
            Path = TEXT("/") + Path;
        }
        Path.ReplaceInline(TEXT("{id}"), *SnapshotId);
        return Url + Path;
    }

    static bool CheckServerHasHash(
        const FString& BaseUrl,
        const FString& PathTemplate,
        const FString& Hash,
        bool& OutExists)
    {
        OutExists = false;
        if (BaseUrl.IsEmpty() || PathTemplate.IsEmpty() || Hash.IsEmpty())
        {
            return false;
        }

        FString Url = NormalizeBaseUrl(BaseUrl);

        FString Path = PathTemplate;
        Path.TrimStartAndEndInline();
        if (!Path.StartsWith(TEXT("/")))
        {
            Path = TEXT("/") + Path;
        }
        Path.ReplaceInline(TEXT("{hash}"), *Hash);
        Url += Path;

        FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(true);
        TAtomic<bool> bDone(false);
        TAtomic<bool> bOk(false);

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(Url);
        Request->SetVerb(TEXT("GET"));
        Request->OnProcessRequestComplete().BindLambda(
            [&](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
            {
                if (bSucceeded && Resp.IsValid() && Resp->GetResponseCode() == 200)
                {
                    TSharedPtr<FJsonObject> Root;
                    const FString Body = Resp->GetContentAsString();
                    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
                    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
                    {
                        OutExists = Root->GetBoolField(TEXT("exists"));
                        bOk = true;
                    }
                }
                bDone = true;
                DoneEvent->Trigger();
            });

        Request->ProcessRequest();

        const double Start = FPlatformTime::Seconds();
        while (!bDone && (FPlatformTime::Seconds() - Start) < 5.0)
        {
            FHttpModule::Get().GetHttpManager().Tick(0.01f);
            FPlatformProcess::Sleep(0.01f);
        }

        FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
        if (!bDone)
        {
            UE_LOG(LogAssetSnapshot, Warning, TEXT("GetServerExportFilters: timeout waiting for %s"), *Url);
        }
        if (!bOk && bDone)
        {
            UE_LOG(LogAssetSnapshot, Warning, TEXT("GetServerExportFilters: no usable data from %s"), *Url);
        }
        return bOk;
    }

    static bool ResolveProjectIdFromServer(const FString& BaseUrl, const FString& SourcePath, int32& OutProjectId)
    {
        OutProjectId = 0;
        if (BaseUrl.IsEmpty() || SourcePath.IsEmpty())
        {
            return false;
        }

        FString Url = NormalizeBaseUrl(BaseUrl);
        Url += TEXT("/projects/resolve?source_path=");
        Url += FGenericPlatformHttp::UrlEncode(SourcePath);
        Url += TEXT("&auto_create=1");

        TSharedPtr<FEvent> DoneEvent = MakeShareable(
            FPlatformProcess::GetSynchEventFromPool(true),
            [](FEvent* E) { FPlatformProcess::ReturnSynchEventToPool(E); });
        TSharedRef<TAtomic<bool>> bDone = MakeShared<TAtomic<bool>>(false);
        TSharedRef<TAtomic<bool>> bOk = MakeShared<TAtomic<bool>>(false);
        TSharedRef<TAtomic<bool>> bAbandoned = MakeShared<TAtomic<bool>>(false);

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(Url);
        Request->SetVerb(TEXT("GET"));
        Request->OnProcessRequestComplete().BindLambda(
            [bDone, bOk, bAbandoned, DoneEvent, &OutProjectId](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
            {
                if (*bAbandoned)
                {
                    return;
                }
                if (bSucceeded && Resp.IsValid() && Resp->GetResponseCode() == 200)
                {
                    TSharedPtr<FJsonObject> Root;
                    const FString Body = Resp->GetContentAsString();
                    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
                    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
                    {
                        const TSharedPtr<FJsonValue> Value = Root->TryGetField(TEXT("project_id"));
                        if (Value.IsValid() && Value->Type == EJson::Number)
                        {
                            OutProjectId = (int32)Value->AsNumber();
                            if (OutProjectId > 0)
                            {
                                bOk->Store(true);
                            }
                        }
                    }
                }
                bDone->Store(true);
                DoneEvent->Trigger();
            });

        Request->ProcessRequest();

        const double Start = FPlatformTime::Seconds();
        while (!*bDone && (FPlatformTime::Seconds() - Start) < 5.0)
        {
            FHttpModule::Get().GetHttpManager().Tick(0.01f);
            FPlatformProcess::Sleep(0.01f);
        }

        if (!*bDone)
        {
            bAbandoned->Store(true);
            Request->CancelRequest();
            UE_LOG(LogAssetSnapshot, Warning, TEXT("ResolveProjectIdFromServer: timeout waiting for %s"), *Url);
        }
        return *bOk;
    }

    static bool UploadZipToServer(
        const FString& BaseUrl,
        const FString& PathTemplate,
        const FString& ZipPath,
        int32 ProjectId)
    {
        if (BaseUrl.IsEmpty() || ZipPath.IsEmpty() || ProjectId <= 0)
        {
            return false;
        }

        FString Url = NormalizeBaseUrl(BaseUrl);

        FString Path = PathTemplate;
        Path.TrimStartAndEndInline();
        if (Path.IsEmpty())
        {
            Path = TEXT("/assets/upload");
        }
        if (!Path.StartsWith(TEXT("/")))
        {
            Path = TEXT("/") + Path;
        }
        Url += Path;

        TArray<uint8> ZipData;
        if (!FFileHelper::LoadFileToArray(ZipData, *ZipPath))
        {
            UE_LOG(LogAssetSnapshot, Warning, TEXT("UploadZipToServer: failed to read zip %s"), *ZipPath);
            return false;
        }

        const FString Boundary = FString::Printf(TEXT("----AssetSnapshotBoundary%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString FileName = FPaths::GetCleanFilename(ZipPath);

        auto AppendString = [](TArray<uint8>& Out, const FString& Text)
        {
            FTCHARToUTF8 Utf8(*Text);
            Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        };

        TArray<uint8> Body;
        AppendString(Body, TEXT("--") + Boundary + TEXT("\r\n"));
        AppendString(Body, TEXT("Content-Disposition: form-data; name=\"project_id\"\r\n\r\n"));
        AppendString(Body, FString::FromInt(ProjectId) + TEXT("\r\n"));

        AppendString(Body, TEXT("--") + Boundary + TEXT("\r\n"));
        AppendString(Body, TEXT("Content-Disposition: form-data; name=\"file\"; filename=\"") + FileName + TEXT("\"\r\n"));
        AppendString(Body, TEXT("Content-Type: application/zip\r\n\r\n"));
        Body.Append(ZipData);
        AppendString(Body, TEXT("\r\n--") + Boundary + TEXT("--\r\n"));

        TSharedPtr<FEvent> DoneEvent = MakeShareable(
            FPlatformProcess::GetSynchEventFromPool(true),
            [](FEvent* E) { FPlatformProcess::ReturnSynchEventToPool(E); });
        TSharedRef<TAtomic<bool>> bDone = MakeShared<TAtomic<bool>>(false);
        TSharedRef<TAtomic<bool>> bOk = MakeShared<TAtomic<bool>>(false);
        TSharedRef<TAtomic<bool>> bAbandoned = MakeShared<TAtomic<bool>>(false);

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(Url);
        Request->SetVerb(TEXT("POST"));
        Request->SetHeader(TEXT("Content-Type"), TEXT("multipart/form-data; boundary=") + Boundary);
        Request->SetContent(MoveTemp(Body));
        Request->OnProcessRequestComplete().BindLambda(
            [bDone, bOk, bAbandoned, DoneEvent](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
            {
                if (*bAbandoned)
                {
                    return;
                }
                if (bSucceeded && Resp.IsValid() && Resp->GetResponseCode() == 200)
                {
                    bOk->Store(true);
                }
                bDone->Store(true);
                DoneEvent->Trigger();
            });

        Request->ProcessRequest();

        const double Start = FPlatformTime::Seconds();
        while (!*bDone && (FPlatformTime::Seconds() - Start) < 10.0)
        {
            FHttpModule::Get().GetHttpManager().Tick(0.01f);
            FPlatformProcess::Sleep(0.01f);
        }

        if (!*bDone)
        {
            bAbandoned->Store(true);
            Request->CancelRequest();
            UE_LOG(LogAssetSnapshot, Warning, TEXT("UploadZipToServer: timeout waiting for %s"), *Url);
        }
        return *bOk;
    }

    static void SendUploadEvent(const FString& BaseUrl, const FString& AssetName)
    {
        if (BaseUrl.IsEmpty())
        {
            return;
        }

        FString Url = NormalizeBaseUrl(BaseUrl);
        Url += TEXT("/events/notify");

        const int32 Total = GAssetSnapshotExportTotal;
        const int32 Current = GAssetSnapshotExportCurrent;
        const int32 Percent = Total > 0 ? FMath::RoundToInt((double)Current / (double)Total * 100.0) : 0;

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("batch_id"), GAssetSnapshotExportBatchId);
        Root->SetNumberField(TEXT("current"), Current);
        Root->SetNumberField(TEXT("total"), Total);
        Root->SetNumberField(TEXT("percent"), Percent);
        Root->SetStringField(TEXT("name"), AssetName);
        Root->SetStringField(TEXT("source"), TEXT("plugin"));

        FString Body;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
        FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(Url);
        Request->SetVerb(TEXT("POST"));
        Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
        Request->SetContentAsString(Body);
        Request->ProcessRequest();
    }

static bool GetServerExportFilters(
        const FString& BaseUrl,
        FString& OutInclude,
        FString& OutExclude)
    {
        OutInclude.Reset();
        OutExclude.Reset();
        if (BaseUrl.IsEmpty())
        {
            return false;
        }

        FString Url = NormalizeBaseUrl(BaseUrl);
        Url += TEXT("/settings");

        static double LastFetchAt = 0.0;
        static double FetchStartedAt = 0.0;
        static FString CachedInclude;
        static FString CachedExclude;
        static bool bCacheValid = false;
        static bool bFetchInFlight = false;
        static TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
        static FCriticalSection CacheMutex;

        const double Now = FPlatformTime::Seconds();
        FHttpModule::Get().GetHttpManager().Tick(0.0f);
        {
            FScopeLock Lock(&CacheMutex);
            if (bCacheValid && (Now - LastFetchAt) < 5.0)
            {
                OutInclude = CachedInclude;
                OutExclude = CachedExclude;
                return true;
            }
        }

        if (bFetchInFlight)
        {
            if ((Now - FetchStartedAt) > 5.0)
            {
                bFetchInFlight = false;
            }
            return false;
        }

        bFetchInFlight = true;
        FetchStartedAt = Now;
        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
        ActiveRequest = Request;
        Request->SetURL(Url);
        Request->SetVerb(TEXT("GET"));
        Request->OnProcessRequestComplete().BindLambda(
            [&](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
            {
                FString Include;
                FString Exclude;
                const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : -1;
                const FString Body = Resp.IsValid() ? Resp->GetContentAsString() : FString();
                bool bParsed = false;
                if (bSucceeded && Resp.IsValid() && Code == 200)
                {
                    TSharedPtr<FJsonObject> Root;
                    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
                    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
                    {
                        if (const TSharedPtr<FJsonValue> SkipVal = Root->TryGetField(TEXT("skip_export_if_on_server")))
                        {
                            bool bParsedSkip = false;
                            bool bEnabled = true;
                            if (SkipVal->Type == EJson::Boolean)
                            {
                                bEnabled = SkipVal->AsBool();
                                bParsedSkip = true;
                            }
                            else if (SkipVal->Type == EJson::String)
                            {
                                const FString Raw = SkipVal->AsString().ToLower();
                                if (Raw == TEXT("1") || Raw == TEXT("true") || Raw == TEXT("yes") || Raw == TEXT("on"))
                                {
                                    bEnabled = true;
                                    bParsedSkip = true;
                                }
                                else if (Raw == TEXT("0") || Raw == TEXT("false") || Raw == TEXT("no") || Raw == TEXT("off"))
                                {
                                    bEnabled = false;
                                    bParsedSkip = true;
                                }
                            }
                            else if (SkipVal->Type == EJson::Number)
                            {
                                bEnabled = SkipVal->AsNumber() != 0.0;
                                bParsedSkip = true;
                            }

                            if (bParsedSkip)
                            {
                                GAssetSnapshotServerSkipKnown = true;
                                GAssetSnapshotServerSkipEnabled = bEnabled;
                            }
                        }

                        auto JoinArray = [](const TArray<TSharedPtr<FJsonValue>>& Values) -> FString
                        {
                            TArray<FString> Parts;
                            for (const TSharedPtr<FJsonValue>& V : Values)
                            {
                                if (!V.IsValid())
                                {
                                    continue;
                                }
                                const FString S = V->AsString().TrimStartAndEnd();
                                if (!S.IsEmpty())
                                {
                                    Parts.Add(S);
                                }
                            }
                            return FString::Join(Parts, TEXT(","));
                        };

                        if (const TSharedPtr<FJsonValue> IncludeVal = Root->TryGetField(TEXT("export_include_types")))
                        {
                            if (IncludeVal->Type == EJson::Array)
                            {
                                Include = JoinArray(IncludeVal->AsArray());
                            }
                            else if (IncludeVal->Type == EJson::String)
                            {
                                Include = IncludeVal->AsString();
                            }
                        }
                        if (const TSharedPtr<FJsonValue> ExcludeVal = Root->TryGetField(TEXT("export_exclude_types")))
                        {
                            if (ExcludeVal->Type == EJson::Array)
                            {
                                Exclude = JoinArray(ExcludeVal->AsArray());
                            }
                            else if (ExcludeVal->Type == EJson::String)
                            {
                                Exclude = ExcludeVal->AsString();
                            }
                        }
                        bParsed = true;
                        UE_LOG(LogAssetSnapshot, Log, TEXT("GetServerExportFilters: parsed include='%s' exclude='%s'"), *Include, *Exclude);
                    }
                    else
                    {
                        UE_LOG(LogAssetSnapshot, Warning, TEXT("GetServerExportFilters: JSON parse failed body='%s'"), *Body);
                    }
                }
                else
                {
                    UE_LOG(LogAssetSnapshot, Warning, TEXT("GetServerExportFilters: request failed ok=%s code=%d"),
                        bSucceeded ? TEXT("true") : TEXT("false"),
                        Code);
                }

                {
                    FScopeLock Lock(&CacheMutex);
                    CachedInclude = Include;
                    CachedExclude = Exclude;
                    LastFetchAt = FPlatformTime::Seconds();
                    bCacheValid = bParsed;
                }
                bFetchInFlight = false;
                ActiveRequest.Reset();
            });

        Request->ProcessRequest();
        // Short tick to allow completion without stalling export too long.
        for (int32 i = 0; i < 100; ++i)
        {
            FHttpModule::Get().GetHttpManager().Tick(0.01f);
            FPlatformProcess::Sleep(0.01f);
            {
                FScopeLock Lock(&CacheMutex);
                if (bCacheValid)
                {
                    OutInclude = CachedInclude;
                    OutExclude = CachedExclude;
                    return true;
                }
            }
        }
        return false;
    }

    static bool IsServerSkipExportEnabled(const FString& BaseUrl, bool& OutEnabled)
    {
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(BaseUrl);
        OutEnabled = Server.bSkipExportIfOnServer;
        return Server.bAvailable;
    }

    static bool PackageToMainFileAbs(const FString& PackageName, FString& OutAbs)
    {
        FString AbsUAsset = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        if (FPaths::FileExists(AbsUAsset))
        {
            OutAbs = AbsUAsset;
            return true;
        }

        FString AbsUMap = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
        if (FPaths::FileExists(AbsUMap))
        {
            OutAbs = AbsUMap;
            return true;
        }

        OutAbs = AbsUAsset; // best effort
        return false;
    }

    static void AddIfExistsAllowlisted(const FString& AbsPath, const FString& ContentDir, TSet<FString>& SeenRel, TArray<FString>& OutRel, TArray<FString>& OutAbs, int64& TotalBytes)
    {
        if (!FPaths::FileExists(AbsPath))
        {
            return;
        }

        FString Rel = AbsPath;
        FPaths::MakePathRelativeTo(Rel, *ContentDir);
        Rel = NormalizeRelPath(Rel);

        const FString Ext = FPaths::GetExtension(Rel, true).ToLower();
        const bool bAllowed = (Ext == TEXT(".uasset") || Ext == TEXT(".uexp") || Ext == TEXT(".ubulk") || Ext == TEXT(".uptnl") || Ext == TEXT(".umap"));
        if (!bAllowed)
        {
            return;
        }

        if (SeenRel.Contains(Rel))
        {
            return;
        }

        SeenRel.Add(Rel);
        OutRel.Add(Rel);
        OutAbs.Add(AbsPath);
        TotalBytes += (int64)IFileManager::Get().FileSize(*AbsPath);
    }

    static void GatherFilesOnDiskForPackage(const FString& PackageName, TSet<FString>& SeenRel, TArray<FString>& OutRel, TArray<FString>& OutAbs, int64& TotalBytes)
    {
        const FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());

        FString MainAbs;
        PackageToMainFileAbs(PackageName, MainAbs);
        const FString BaseNoExt = FPaths::ChangeExtension(MainAbs, TEXT(""));

        AddIfExistsAllowlisted(MainAbs, ContentDir, SeenRel, OutRel, OutAbs, TotalBytes);
        AddIfExistsAllowlisted(BaseNoExt + TEXT(".uexp"), ContentDir, SeenRel, OutRel, OutAbs, TotalBytes);
        AddIfExistsAllowlisted(BaseNoExt + TEXT(".ubulk"), ContentDir, SeenRel, OutRel, OutAbs, TotalBytes);
        AddIfExistsAllowlisted(BaseNoExt + TEXT(".uptnl"), ContentDir, SeenRel, OutRel, OutAbs, TotalBytes);
    }

    static void GatherGameDependenciesPackages(const FString& RootPackage, TArray<FString>& OutPackages)
{
    OutPackages.Reset();

    // We only care about /Game packages for recovery.
    if (!RootPackage.StartsWith(TEXT("/Game/")))
    {
        OutPackages.Add(RootPackage);
        return;
    }

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    // Use the default dependency query here (UE versions differ in how flags are interpreted).

    TArray<FName> Queue;
    TSet<FName> Seen;

    const FName RootPkg(*RootPackage);
    Queue.Add(RootPkg);
    Seen.Add(RootPkg);

    for (int32 Qi = 0; Qi < Queue.Num(); ++Qi)
    {
        const FName Cur = Queue[Qi];

        TArray<FName> Deps;
        AR.GetDependencies(Cur, Deps, UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName& Dep : Deps)
        {
            const FString DepStr = Dep.ToString();
            if (!DepStr.StartsWith(TEXT("/Game/")))
            {
                continue;
            }
            if (Seen.Contains(Dep))
            {
                continue;
            }

            Seen.Add(Dep);
            Queue.Add(Dep);
        }
    }

    // Deterministic output.
    for (const FName& Pkg : Seen)
    {
        OutPackages.Add(Pkg.ToString());
    }
    OutPackages.Sort();
}

    struct FMeshStats
    {
        int64 Triangles = 0;
        int64 Vertices = 0;
        int32 LODs = 0;
        bool bNaniteEnabled = false;
        FString CollisionComplexity;
        FVector ApproxSize = FVector::ZeroVector; // cm
    };

    static FMeshStats GetStaticMeshStats(UStaticMesh* SM)
    {
        FMeshStats S;
        if (!SM)
        {
            return S;
        }

        const FBoxSphereBounds B = SM->GetBounds();
        S.ApproxSize = B.BoxExtent * 2.0f;

        if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
        {
            const FStaticMeshLODResources& LOD0 = SM->GetRenderData()->LODResources[0];
            S.Triangles = (int64)LOD0.GetNumTriangles();
            S.Vertices = (int64)LOD0.GetNumVertices();
            S.LODs = SM->GetRenderData()->LODResources.Num();
        }

#if WITH_EDITOR
    #if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
        S.bNaniteEnabled = SM->GetNaniteSettings().bEnabled || SM->IsNaniteForceEnabled();
    #else
        S.bNaniteEnabled = SM->NaniteSettings.bEnabled || SM->IsNaniteForceEnabled();
    #endif
#else
        S.bNaniteEnabled = false;
#endif

        if (UBodySetup* BS = SM->GetBodySetup())
        {
            switch (BS->CollisionTraceFlag)
            {
            case CTF_UseDefault: S.CollisionComplexity = TEXT("UseDefault"); break;
            case CTF_UseSimpleAsComplex: S.CollisionComplexity = TEXT("UseSimpleAsComplex"); break;
            case CTF_UseComplexAsSimple: S.CollisionComplexity = TEXT("UseComplexAsSimple"); break;
            case CTF_UseSimpleAndComplex: S.CollisionComplexity = TEXT("UseSimpleAndComplex"); break;
            default: S.CollisionComplexity = TEXT("Unknown"); break;
            }
        }
        else
        {
            S.CollisionComplexity = TEXT("None");
        }

        return S;
    }

    static FMeshStats GetSkeletalMeshStats(USkeletalMesh* SK)
    {
        FMeshStats S;
        if (!SK)
        {
            return S;
        }

        const FBoxSphereBounds B = SK->GetBounds();
        S.ApproxSize = B.BoxExtent * 2.0f;

        const FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
        if (RD && RD->LODRenderData.Num() > 0)
        {
            S.LODs = RD->LODRenderData.Num();
            const FSkeletalMeshLODRenderData& LOD0 = RD->LODRenderData[0];
            S.Triangles = (int64)LOD0.GetTotalFaces();
            S.Vertices = (int64)LOD0.GetNumVertices();
        }

        S.bNaniteEnabled = false;
        S.CollisionComplexity = TEXT("N/A");
        return S;
    }

    static float ComputeCameraDistanceFromBounds(float SphereRadius, float FovDeg, float Padding)
    {
        const float HalfFovRad = FMath::DegreesToRadians(FovDeg * 0.5f);
        const float Dist = (SphereRadius / FMath::Tan(HalfFovRad)) * Padding;
        return FMath::Max(50.0f, Dist);
    }

    static void WarmupWorld(UWorld* World, float Seconds)
    {
        if (!World || Seconds <= 0.f)
        {
            return;
        }

        const float Dt = 1.f / 60.f;
        const int32 Steps = FMath::Max(1, FMath::CeilToInt(Seconds / Dt));
        for (int32 i = 0; i < Steps; ++i)
        {
            World->Tick(LEVELTICK_All, Dt);
            IStreamingManager::Get().Tick(Dt);
            FlushRenderingCommands();
        }
        IStreamingManager::Get().BlockTillAllRequestsFinished(Seconds, false);
        FPlatformProcess::Sleep(kWarmupPauseSeconds);

    }

    static void BlockStreamingAndCompiles(UWorld* World)
    {
        if (!World)
        {
            return;
        }

        const float Dt = 1.f / 60.f;
        for (int32 i = 0; i < 4; ++i)
        {
            World->Tick(LEVELTICK_All, Dt);
            IStreamingManager::Get().Tick(Dt);
        }

        IStreamingManager::Get().BlockTillAllRequestsFinished(kWarmupSeconds, false);
        FlushRenderingCommands();
#if WITH_EDITOR
        FAssetCompilingManager::Get().FinishAllCompilation();
#endif
    }

    static int32 ClampPreviewResolution(int32 InResolution)
    {
        return FMath::Clamp(InResolution, 128, 2048);
    }

    static int32 GetMaterialPreviewResolution(UMaterialInterface* Mat, int32 Fallback)
    {
        if (!Mat)
        {
            return ClampPreviewResolution(Fallback);
        }

        TArray<UTexture*> Used;
        Mat->GetUsedTextures(Used);

        int32 MaxDim = 0;
        for (UTexture* Tex : Used)
        {
            if (!Tex)
            {
                continue;
            }
            MaxDim = FMath::Max(MaxDim, (int32)Tex->GetSurfaceWidth());
            MaxDim = FMath::Max(MaxDim, (int32)Tex->GetSurfaceHeight());
        }

        const int32 Base = (MaxDim > 0) ? MaxDim : Fallback;
        return FMath::Max(kMinMaterialResolution, ClampPreviewResolution(Base));
    }

    static int32 GetTexturePreviewResolution(UTexture2D* Tex, int32 Fallback)
    {
        return ClampPreviewResolution(kTexturePreviewResolution);
    }

    static void ComputeTextureCaptureSize(UTexture2D* Tex, int32 Fallback, int32& OutWidth, int32& OutHeight)
    {
        const int32 Target = ClampPreviewResolution(kTexturePreviewResolution);
        OutWidth = Target;
        OutHeight = Target;
    }

    // Pick a stable "good for vision" view direction.
    // Heuristic: look along the *thinnest* horizontal axis (X or Y). That usually produces
    // a readable side-profile (e.g. pistols) instead of a front/back view.
    static FVector ChooseStableViewDirFromBoxExtent(const FVector& BoxExtent)
    {
        const bool bUseX = (BoxExtent.X < BoxExtent.Y);
        const FVector Horizontal = bUseX ? FVector(-1.f, 0.f, 0.f) : FVector(0.f, -1.f, 0.f);
        return (Horizontal + FVector(0.f, 0.f, 0.05f)).GetSafeNormal();
    }


    static void SetupDefaultLights(FPreviewScene& Scene)
    {
        // Enhanced 3-point lighting for 360° view
        // Key Light (Main) - Front-left, brighter
        UDirectionalLightComponent* Key = NewObject<UDirectionalLightComponent>(GetTransientPackage());
        Key->Intensity = 2.0f;  // Increased from 1.25
		Key->SetCastShadows(true);
        Key->LightColor = FColor::White;
        Scene.AddComponent(Key, FTransform(FRotator(-30.f, 45.f, 0.f), FVector::ZeroVector));

        // Fill Light (Secondary) - Front-right
        UDirectionalLightComponent* Fill = NewObject<UDirectionalLightComponent>(GetTransientPackage());
        Fill->Intensity = 1.2f;  // Increased from 0.625
		Fill->SetCastShadows(false);
        Fill->LightColor = FColor::White;
        Scene.AddComponent(Fill, FTransform(FRotator(-20.f, -120.f, 0.f), FVector::ZeroVector));

        // Rim Light (Back) - From behind for edge definition
        UDirectionalLightComponent* Rim = NewObject<UDirectionalLightComponent>(GetTransientPackage());
        Rim->Intensity = 1.0f;  // Increased from 0.75
		Rim->SetCastShadows(false);
        Rim->LightColor = FColor::White;
        Scene.AddComponent(Rim, FTransform(FRotator(20.f, 180.f, 0.f), FVector::ZeroVector));

        // Additional Side Lights for 360° coverage
        UDirectionalLightComponent* SideLeft = NewObject<UDirectionalLightComponent>(GetTransientPackage());
        SideLeft->Intensity = 0.8f;
		SideLeft->SetCastShadows(false);
        SideLeft->LightColor = FColor::White;
        Scene.AddComponent(SideLeft, FTransform(FRotator(-25.f, -90.f, 0.f), FVector::ZeroVector));

        UDirectionalLightComponent* SideRight = NewObject<UDirectionalLightComponent>(GetTransientPackage());
        SideRight->Intensity = 0.8f;
		SideRight->SetCastShadows(false);
        SideRight->LightColor = FColor::White;
        Scene.AddComponent(SideRight, FTransform(FRotator(-25.f, 90.f, 0.f), FVector::ZeroVector));

        // Sky Light - Overall ambient for consistent 360° lighting
        USkyLightComponent* Sky = NewObject<USkyLightComponent>(GetTransientPackage());
        Sky->Intensity = 0.5f;  // Increased from 0.25
        Sky->LightColor = FColor::White;
        Sky->bLowerHemisphereIsBlack = false;
        Scene.AddComponent(Sky, FTransform::Identity);
    }

    static bool InitMaterialCaptureContext(FMaterialCaptureContext& Ctx)
    {
        SetupDefaultLights(Ctx.Scene);

        Ctx.World = Ctx.Scene.GetWorld();
        if (!Ctx.World)
        {
            return false;
        }

        UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
        if (!Sphere)
        {
            return false;
        }

        Ctx.Comp = NewObject<UStaticMeshComponent>(GetTransientPackage());
        Ctx.Comp->SetStaticMesh(Sphere);
        Ctx.Comp->SetMobility(EComponentMobility::Movable);
        Ctx.Comp->RegisterComponentWithWorld(Ctx.World);
        Ctx.Comp->SetWorldScale3D(FVector(2.0f));
        Ctx.Comp->SetWorldRotation(FRotator(0.f, 270.f, 0.f));  // 270Жј
        Ctx.Comp->UpdateBounds();

        Ctx.Scene.AddComponent(Ctx.Comp, FTransform::Identity);
        Ctx.Comp->MarkRenderStateDirty();

        const FBoxSphereBounds B = Ctx.Comp->Bounds;
        const float Radius = B.SphereRadius;
        Ctx.Distance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, 1.05f);
        Ctx.ViewDir = FVector(1.f, 0.f, 0.f);
        return true;
    }

    static void ForceComponentTexturesResident(UPrimitiveComponent* Comp)
{
    if (!Comp)
    {
        return;
    }

    TSet<UTexture*> UniqueTextures;

    Comp->bForceMipStreaming = true;

    TArray<UTexture*> Used;
    Comp->GetUsedTextures(Used, EMaterialQualityLevel::High);
    for (UTexture* Tex : Used)
    {
        if (Tex)
        {
            UniqueTextures.Add(Tex);
        }
    }

    for (UTexture* Tex : UniqueTextures)
    {
        Tex->SetForceMipLevelsToBeResident(30.0f);
    }

    // Let the render thread process residency requests.
    FlushRenderingCommands();

    for (UTexture* Tex : UniqueTextures)
    {
        Tex->WaitForStreaming();
    }

    IStreamingManager::Get().BlockTillAllRequestsFinished(kWarmupSeconds, false);
    Comp->MarkRenderStateDirty();
}

    static bool CapturePreviewSceneToWebPBytes(
        FPreviewScene& Scene,
        const FVector& LookAt,
        float Distance,
        float FovDeg,
        int32 Resolution,
        TArray<uint8>& OutWebP,
        const FVector& ViewDirFromLookAt,
        float YawRotationDegrees = 0.0f)  // 360° view rotation
    {
        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        AActor* CaptureActor = World->SpawnActor<AActor>();
        USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(CaptureActor);
        Capture->RegisterComponentWithWorld(World);
        CaptureActor->SetRootComponent(Capture);

        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
        RT->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
        RT->ClearColor = FLinearColor::Black;
        RT->TargetGamma = 1.8f;  // Compromise between too dark (1.0) and too bright (2.2)
        RT->UpdateResourceImmediate(true);

        Capture->TextureTarget = RT;
        Capture->FOVAngle = FovDeg;
        Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

        // Make sure post process / exposure doesn't blow out and doesn't add noise
        Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
        Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
        Capture->PostProcessSettings.AutoExposureBias = 0.0f;

        Capture->PostProcessSettings.bOverride_MotionBlurAmount = true;
        Capture->PostProcessSettings.MotionBlurAmount = 0.0f;
        Capture->PostProcessSettings.bOverride_VignetteIntensity = true;
        Capture->PostProcessSettings.VignetteIntensity = 0.0f;
        Capture->PostProcessSettings.bOverride_SceneFringeIntensity = true;
        Capture->PostProcessSettings.SceneFringeIntensity = 0.0f;

        // Position camera using a stable view direction
        FVector ViewDir = ViewDirFromLookAt.IsNearlyZero() ? FVector(0.f, -1.f, 0.f) : ViewDirFromLookAt.GetSafeNormal();
        
        // Rotate camera around asset for 360° view
        if (FMath::Abs(YawRotationDegrees) > 0.01f)
        {
            FRotator Rotation(0.f, YawRotationDegrees, 0.f);
            ViewDir = Rotation.RotateVector(ViewDir);
        }
        
        const FVector CamPos = LookAt + (ViewDir * Distance);
        const FRotator CamRot = (LookAt - CamPos).Rotation();
        CaptureActor->SetActorLocation(CamPos);
        CaptureActor->SetActorRotation(CamRot);

#if WITH_EDITOR
        FAssetCompilingManager::Get().FinishAllCompilation();
#endif

        // NO WARMUP - just capture
        // Warmup is handled by frame discarding in multi-frame capture
        Capture->CaptureScene();
        FlushRenderingCommands();

        FTextureRenderTargetResource* Res = RT->GameThread_GetRenderTargetResource();
        if (!Res)
        {
            return false;
        }

        TArray<FColor> Pixels;
        Pixels.SetNumUninitialized(Resolution * Resolution);

        FReadSurfaceDataFlags Flags(RCM_UNorm);
        Flags.SetLinearToGamma(true);
        const bool bReadOk = Res->ReadPixels(Pixels, Flags);
        if (!bReadOk)
        {
            CaptureActor->Destroy();
            return false;
        }

        if (!EncodeWebPFromBGRA(Pixels, Resolution, Resolution, OutWebP))
        {
            CaptureActor->Destroy();
            return false;
        }

        CaptureActor->Destroy();
        return true;
    }

    static bool CaptureStaticMeshMultiFrame(UStaticMesh* SM, int32 Resolution, TArray<FZipEntry>& OutFrames, float& OutDistance)
    {
        if (!SM)
        {
            return false;
        }

        FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage());
        Comp->SetStaticMesh(SM);
        Comp->SetMobility(EComponentMobility::Movable);
        Comp->RegisterComponentWithWorld(World);
        Comp->SetForcedLodModel(1);

        Scene.AddComponent(Comp, FTransform::Identity);
        Comp->UpdateBounds();

        const FVector CenterOffset = -Comp->Bounds.Origin;
        const FVector ViewDir = FVector(0.f, -1.f, 0.05f).GetSafeNormal();
        Comp->SetWorldLocation(CenterOffset);
        
        FRotator BaseRotation = FRotationMatrix::MakeFromX(-ViewDir).Rotator();
        BaseRotation.Yaw += 90.f;  // 90° rotation
        Comp->SetWorldRotation(BaseRotation);
        Comp->UpdateBounds();

        // Ensure textures are resident before capture (prevents low-mip blur on first pass).
        ForceComponentTexturesResident(Comp);

        const float Radius = Comp->Bounds.SphereRadius;
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, kDistancePadding);

        OutFrames.Reset();
        const int32 FramesToKeep = GetStaticMeshFrameCount();
        const int32 FramesToDiscard = GetCapture360DiscardCount();
        const int32 FramesTotal = FramesToKeep + FramesToDiscard;
        OutFrames.Reserve(FramesToKeep);

        // ============================================================================
        // 1 SECOND PAUSE BEFORE SHOOTING - Let textures/shaders load!
        // ============================================================================
        UE_LOG(LogAssetSnapshot, Log, TEXT("Pausing %.1f seconds before capture..."), kCapture360PauseBeforeShoot);
        
        // Tick world multiple times during pause
        const float PauseTickInterval = 0.5f;
        const int32 NumPauseTicks = (int32)(kCapture360PauseBeforeShoot / PauseTickInterval);
        
        for (int32 PauseTick = 0; PauseTick < NumPauseTicks; ++PauseTick)
        {
            World->Tick(LEVELTICK_All, PauseTickInterval);
            Comp->MarkRenderStateDirty();
            FlushRenderingCommands();
            FPlatformProcess::Sleep(PauseTickInterval);
        }
        
        UE_LOG(LogAssetSnapshot, Log, TEXT("Pause complete, starting capture..."));
        // ============================================================================

        // Now capture frames (skip first 3, keep rest)
        for (int32 i = 0; i < FramesTotal; ++i)
        {
            if (i > 0)
            {
                World->Tick(LEVELTICK_All, kCapture360FrameInterval);
                Comp->MarkRenderStateDirty();
                FlushRenderingCommands();
                FPlatformProcess::Sleep(kCapture360FrameInterval);
            }

            // 360° camera rotation
            float CameraYaw = 0.0f;
            if (i >= FramesToDiscard)
            {
                const int32 FrameIndex = i - FramesToDiscard;
                CameraYaw = (360.0f / (float)FramesToKeep) * FrameIndex;
            }

            TArray<uint8> WebP;
            if (CapturePreviewSceneToWebPBytes(Scene, Comp->Bounds.Origin, OutDistance, kDefaultFov, Resolution, WebP, ViewDir, CameraYaw))
            {
                if (i >= FramesToDiscard)
                {
                    FZipEntry Frame;
                    Frame.NameInZip = FString::Printf(TEXT("%d.webp"), OutFrames.Num());
                    Frame.Data = MoveTemp(WebP);
                    OutFrames.Add(MoveTemp(Frame));
                }
            }
        }

        return OutFrames.Num() > 0;
    }

    static bool CaptureStaticMesh(UStaticMesh* SM, int32 Resolution, TArray<uint8>& OutWebP, float& OutDistance)
    {
        // Legacy single-frame wrapper for backward compatibility
        TArray<FZipEntry> Frames;
        if (!CaptureStaticMeshMultiFrame(SM, Resolution, Frames, OutDistance))
        {
            return false;
        }
        if (Frames.Num() > 0)
        {
            OutWebP = MoveTemp(Frames[0].Data);  // Return first frame
            return true;
        }
        return false;
    }

    static bool CaptureSkeletalMesh(USkeletalMesh* SK, int32 Resolution, TArray<uint8>& OutWebP, float& OutDistance)
    {
        if (!SK)
        {
            return false;
        }

		FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        USkeletalMeshComponent* Comp = NewObject<USkeletalMeshComponent>(GetTransientPackage());
        Comp->SetSkeletalMesh(SK);
        Comp->SetMobility(EComponentMobility::Movable);
        Comp->RegisterComponentWithWorld(World);
        Comp->SetForcedLOD(1);

        Scene.AddComponent(Comp, FTransform::Identity);
        Comp->UpdateBounds();

        const FVector CenterOffset = -Comp->Bounds.Origin;
        const FVector ViewDir = FVector(0.f, -1.f, 0.05f).GetSafeNormal();  // Y-axis
        Comp->SetWorldLocation(CenterOffset);
        
        // Rotate mesh: base rotation + 90° around Z-axis
        FRotator BaseRotation = FRotationMatrix::MakeFromX(-ViewDir).Rotator();
        BaseRotation.Yaw += 90.f;  // Add 90° rotation towards camera
        Comp->SetWorldRotation(BaseRotation);
        Comp->UpdateBounds();

        // Ensure textures are resident before capture (prevents silver previews while streaming is pending).
        ForceComponentTexturesResident(Comp);

        const float Radius = Comp->Bounds.SphereRadius;
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, kDistancePadding);
        return CapturePreviewSceneToWebPBytes(Scene, Comp->Bounds.Origin, OutDistance, kDefaultFov, Resolution, OutWebP, ViewDir);
    }

    static bool CaptureSkeletalMeshMultiFrame(USkeletalMesh* SK, int32 Resolution, TArray<FZipEntry>& OutFrames, float& OutDistance)
    {
        if (!SK)
        {
            return false;
        }

        FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        USkeletalMeshComponent* Comp = NewObject<USkeletalMeshComponent>(GetTransientPackage());
        Comp->SetSkeletalMesh(SK);
        Comp->SetMobility(EComponentMobility::Movable);
        Comp->RegisterComponentWithWorld(World);
        Comp->SetForcedLOD(1);

        Scene.AddComponent(Comp, FTransform::Identity);
        Comp->UpdateBounds();

        const FVector CenterOffset = -Comp->Bounds.Origin;
        const FVector ViewDir = FVector(0.f, -1.f, 0.05f).GetSafeNormal();
        Comp->SetWorldLocation(CenterOffset);
        
        FRotator BaseRotation = FRotationMatrix::MakeFromX(-ViewDir).Rotator();
        BaseRotation.Yaw += 90.f;  // 90° rotation
        Comp->SetWorldRotation(BaseRotation);
        Comp->UpdateBounds();

        // Ensure textures are resident before capture (prevents low-mip blur on first pass).
        ForceComponentTexturesResident(Comp);

        const float Radius = Comp->Bounds.SphereRadius;
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, kDistancePadding);

        OutFrames.Reset();
        const int32 FramesToKeep = GetSkeletalMeshFrameCount();
        const int32 FramesToDiscard = GetCapture360DiscardCount();
        const int32 FramesTotal = FramesToKeep + FramesToDiscard;
        OutFrames.Reserve(FramesToKeep);

        // ============================================================================
        // 1 SECOND PAUSE BEFORE SHOOTING - Let textures/shaders load!
        // ============================================================================
        UE_LOG(LogAssetSnapshot, Log, TEXT("Pausing %.1f seconds before capture..."), kCapture360PauseBeforeShoot);
        
        // Tick world multiple times during pause
        const float PauseTickInterval = 0.5f;
        const int32 NumPauseTicks = (int32)(kCapture360PauseBeforeShoot / PauseTickInterval);
        
        for (int32 PauseTick = 0; PauseTick < NumPauseTicks; ++PauseTick)
        {
            World->Tick(LEVELTICK_All, PauseTickInterval);
            Comp->MarkRenderStateDirty();
            FlushRenderingCommands();
            FPlatformProcess::Sleep(PauseTickInterval);
        }
        
        UE_LOG(LogAssetSnapshot, Log, TEXT("Pause complete, starting capture..."));
        // ============================================================================

        // Now capture frames (skip first 3, keep rest)
        for (int32 i = 0; i < FramesTotal; ++i)
        {
            if (i > 0)
            {
                World->Tick(LEVELTICK_All, kCapture360FrameInterval);
                Comp->MarkRenderStateDirty();
                FlushRenderingCommands();
                FPlatformProcess::Sleep(kCapture360FrameInterval);
            }

            // 360° camera rotation
            float CameraYaw = 0.0f;
            if (i >= FramesToDiscard)
            {
                const int32 FrameIndex = i - FramesToDiscard;
                CameraYaw = (360.0f / (float)FramesToKeep) * FrameIndex;
            }

            TArray<uint8> WebP;
            if (CapturePreviewSceneToWebPBytes(Scene, Comp->Bounds.Origin, OutDistance, kDefaultFov, Resolution, WebP, ViewDir, CameraYaw))
            {
                if (i >= FramesToDiscard)
                {
                    FZipEntry Frame;
                    Frame.NameInZip = FString::Printf(TEXT("%d.webp"), OutFrames.Num());
                    Frame.Data = MoveTemp(WebP);
                    OutFrames.Add(MoveTemp(Frame));
                }
            }
        }

        return OutFrames.Num() > 0;
    }

    static bool CaptureMaterialOnSharedSphereMultiFrame(
        FMaterialCaptureContext& Ctx,
        UMaterialInterface* Mat,
        int32 Resolution,
        TArray<FZipEntry>& OutFrames,
        float& OutDistance,
        bool& OutLowQuality)
    {
        OutLowQuality = false;
        if (!Mat || !Ctx.Comp || !Ctx.World)
        {
            return false;
        }

        auto DoCapturePass = [&](TArray<FZipEntry>& Frames, bool& bLowQuality)
        {
            Frames.Reset();
            const int32 FramesTotal = GetMaterialFrameCount();
            Frames.Reserve(FramesTotal);

            UE_LOG(LogAssetSnapshot, Log, TEXT("Pausing %.1f seconds before capture..."), kCaptureMaterialPauseBeforeShoot);
            const float PauseTickInterval = 0.5f;
            const int32 NumPauseTicks = (int32)(kCaptureMaterialPauseBeforeShoot / PauseTickInterval);
            for (int32 PauseTick = 0; PauseTick < NumPauseTicks; ++PauseTick)
            {
                Ctx.World->Tick(LEVELTICK_All, PauseTickInterval);
                Ctx.Comp->MarkRenderStateDirty();
                FlushRenderingCommands();
                FPlatformProcess::Sleep(PauseTickInterval);
            }

            UE_LOG(LogAssetSnapshot, Log, TEXT("Pause complete, starting capture..."));
            BlockStreamingAndCompiles(Ctx.World);

            for (int32 i = 0; i < FramesTotal; ++i)
            {
                if (i > 0)
                {
                    Ctx.World->Tick(LEVELTICK_All, kCaptureMaterialFrameInterval);
                    Ctx.Comp->MarkRenderStateDirty();
                    FlushRenderingCommands();
                    FPlatformProcess::Sleep(kCaptureMaterialFrameInterval);
                }

                TArray<uint8> WebP;
                if (CapturePreviewSceneToWebPBytes(Ctx.Scene, Ctx.Comp->Bounds.Origin, Ctx.Distance, kDefaultFov, Resolution, WebP, Ctx.ViewDir, 0.0f))
                {
                    const bool bMeetsQuality = (WebP.Num() >= kMaterialMinWebPBytes);
                    if (!bMeetsQuality)
                    {
                        bLowQuality = true;
                    }

                    UE_LOG(
                        LogAssetSnapshot,
                        Log,
                        TEXT("Material capture %s frame %d: %d bytes%s"),
                        Mat ? *Mat->GetName() : TEXT("<null>"),
                        i,
                        WebP.Num(),
                        bMeetsQuality ? TEXT("") : TEXT(" (low quality)"));

                    FZipEntry Frame;
                    Frame.NameInZip = FString::Printf(TEXT("%d.webp"), Frames.Num());
                    Frame.Data = MoveTemp(WebP);
                    Frames.Add(MoveTemp(Frame));
                }
            }

            return Frames.Num() > 0;
        };

        Ctx.Comp->SetMaterial(0, Mat);
        Ctx.Comp->MarkRenderStateDirty();
        ForceComponentTexturesResident(Ctx.Comp);
        WarmupWorld(Ctx.World, kWarmupSeconds);
        ForceComponentTexturesResident(Ctx.Comp);

        OutDistance = Ctx.Distance;

        TArray<FZipEntry> Frames;
        bool bLowQuality = false;
        const bool bCaptured = DoCapturePass(Frames, bLowQuality);
        if (!bCaptured)
        {
            return false;
        }

        if (bLowQuality)
        {
            // Re-apply material and retry once if any frame is too small.
            Ctx.Comp->SetMaterial(0, Mat);
            Ctx.Comp->MarkRenderStateDirty();
            ForceComponentTexturesResident(Ctx.Comp);
            BlockStreamingAndCompiles(Ctx.World);

            TArray<FZipEntry> RetryFrames;
            bool bRetryLowQuality = false;
            const bool bRetryCaptured = DoCapturePass(RetryFrames, bRetryLowQuality);
            if (bRetryCaptured)
            {
                OutLowQuality = bRetryLowQuality;
                OutFrames = MoveTemp(RetryFrames);
                return true;
            }
        }

        OutLowQuality = bLowQuality;
        OutFrames = MoveTemp(Frames);
        return true;
    }

    static bool CaptureMaterialOnSphereMultiFrame(UMaterialInterface* Mat, int32 Resolution, TArray<FZipEntry>& OutFrames, float& OutDistance, bool& OutLowQuality)
    {
        OutLowQuality = false;
        if (!Mat)
        {
            return false;
        }

        FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
        if (!Sphere)
        {
            return false;
        }

        UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage());
        Comp->SetStaticMesh(Sphere);
        Comp->SetMobility(EComponentMobility::Movable);
        Comp->RegisterComponentWithWorld(World);
        Comp->SetWorldScale3D(FVector(2.0f));
        Comp->SetWorldRotation(FRotator(0.f, 270.f, 0.f));  // 270°
        Comp->UpdateBounds();

        Scene.AddComponent(Comp, FTransform::Identity);
        Comp->SetMaterial(0, Mat);
        Comp->MarkRenderStateDirty();

        // Ensure textures are resident before capture (prevents low-mip blur on first pass).
        ForceComponentTexturesResident(Comp);

        // Give shader compilation/streaming a brief warmup for stubborn materials.
        WarmupWorld(World, kWarmupSeconds);
        ForceComponentTexturesResident(Comp);

        const FBoxSphereBounds B = Comp->Bounds;
        const float Radius = B.SphereRadius;
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, 1.05f);
        const FVector ViewDir = FVector(1.f, 0.f, 0.f);

        OutFrames.Reset();
        const int32 FramesTotal = GetMaterialFrameCount();
        OutFrames.Reserve(FramesTotal);

        // ============================================================================
        // 1 SECOND PAUSE BEFORE SHOOTING - Let material parameters settle!
        // ============================================================================
        UE_LOG(LogAssetSnapshot, Log, TEXT("Pausing %.1f seconds before capture..."), kCaptureMaterialPauseBeforeShoot);
        
        // Tick world multiple times during pause
        const float PauseTickInterval = 0.5f;
        const int32 NumPauseTicks = (int32)(kCaptureMaterialPauseBeforeShoot / PauseTickInterval);
        
        for (int32 PauseTick = 0; PauseTick < NumPauseTicks; ++PauseTick)
        {
            World->Tick(LEVELTICK_All, PauseTickInterval);
            Comp->MarkRenderStateDirty();
            FlushRenderingCommands();
            FPlatformProcess::Sleep(PauseTickInterval);
        }
        
        UE_LOG(LogAssetSnapshot, Log, TEXT("Pause complete, starting capture..."));
        // ============================================================================

        // Hard block on streaming/shader compilation before capture.
        BlockStreamingAndCompiles(World);

        // Now capture frames
        for (int32 i = 0; i < FramesTotal; ++i)
        {
            if (i > 0)
            {
                World->Tick(LEVELTICK_All, kCaptureMaterialFrameInterval);
                Comp->MarkRenderStateDirty();
                FlushRenderingCommands();
                FPlatformProcess::Sleep(kCaptureMaterialFrameInterval);
            }

            // NO camera rotation for materials (static view, animated material)
            TArray<uint8> WebP;
            const bool bCapturedOk = CapturePreviewSceneToWebPBytes(Scene, Comp->Bounds.Origin, OutDistance, kDefaultFov, Resolution, WebP, ViewDir, 0.0f);
            if (bCapturedOk)
            {
                const bool bMeetsQuality = (WebP.Num() >= kMaterialMinWebPBytes);
                if (!bMeetsQuality)
                {
                    OutLowQuality = true;
                }

                UE_LOG(
                    LogAssetSnapshot,
                    Log,
                    TEXT("Material capture %s frame %d: %d bytes%s"),
                    Mat ? *Mat->GetName() : TEXT("<null>"),
                    i,
                    WebP.Num(),
                    bMeetsQuality ? TEXT("") : TEXT(" (low quality)"));

                FZipEntry Frame;
                Frame.NameInZip = FString::Printf(TEXT("%d.webp"), OutFrames.Num());
                Frame.Data = MoveTemp(WebP);
                OutFrames.Add(MoveTemp(Frame));
            }
        }

        return OutFrames.Num() > 0;
    }

    static bool CaptureMaterialOnCube(UMaterialInterface* Mat, int32 Resolution, TArray<uint8>& OutWebP, float& OutDistance)
    {
        if (!Mat)
        {
            return false;
        }

		FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        // Use SPHERE for materials, not cube
        UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
        if (!Sphere)
        {
            return false;
        }

        UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage());
        Comp->SetStaticMesh(Sphere);
        Comp->SetMobility(EComponentMobility::Movable);
        Comp->RegisterComponentWithWorld(World);
        Comp->SetWorldScale3D(FVector(2.0f));
        Comp->SetWorldRotation(FRotator(0.f, 270.f, 0.f));  // 270° (was 90°, +180° correction)
        Comp->UpdateBounds();

        Scene.AddComponent(Comp, FTransform::Identity);
        Comp->SetMaterial(0, Mat);
        Comp->MarkRenderStateDirty();

        // Force textures
        ForceComponentTexturesResident(Comp);

        const FBoxSphereBounds B = Comp->Bounds;
        const float Radius = B.SphereRadius;
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, 1.05f);
        const FVector ViewDir = FVector(1.f, 0.f, 0.f);
        return CapturePreviewSceneToWebPBytes(Scene, Comp->Bounds.Origin, OutDistance, kDefaultFov, Resolution, OutWebP, ViewDir);
    }

    static bool CaptureTexture2D(UTexture2D* Tex, int32 Resolution, TArray<uint8>& OutWebP)
    {
        if (!Tex)
        {
            return false;
        }

        Tex->SetForceMipLevelsToBeResident(30.0f);
        Tex->WaitForStreaming();

        int32 Width = 0;
        int32 Height = 0;
        ComputeTextureCaptureSize(Tex, Resolution, Width, Height);

        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
        RT->InitCustomFormat(Width, Height, PF_B8G8R8A8, false);
        RT->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
        RT->TargetGamma = 1.8f;  // Compromise between too dark (1.0) and too bright (2.2)
        RT->UpdateResourceImmediate(true);

        FTextureRenderTargetResource* RTRes = RT->GameThread_GetRenderTargetResource();
        if (!RTRes)
        {
            return false;
        }

        // Draw the texture onto the render target using a Canvas (no custom material needed)
        FCanvas Canvas(RTRes, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
        Canvas.Clear(RT->ClearColor);

        FCanvasTileItem Tile(FVector2D(0, 0), Tex->GetResource(), FVector2D(Width, Height), FLinearColor::White);
        Tile.BlendMode = SE_BLEND_Opaque;
        Canvas.DrawItem(Tile);
        Canvas.Flush_GameThread();
        FlushRenderingCommands();

        TArray<FColor> Pixels;
        Pixels.SetNumUninitialized(Width * Height);
        FReadSurfaceDataFlags Flags(RCM_UNorm);
        Flags.SetLinearToGamma(true);
        const bool bOk = RTRes->ReadPixels(Pixels, Flags);
        if (!bOk)
        {
            return false;
        }

        if (!EncodeWebPFromBGRA(Pixels, Width, Height, OutWebP))
        {
            return false;
        }
        return true;
    }

    static bool CaptureBlueprint(UBlueprint* BP, int32 Resolution, TArray<uint8>& OutWebP, float& OutDistance)
    {
        if (!BP || !BP->GeneratedClass)
        {
            return false;
        }

        UClass* Cls = BP->GeneratedClass;
        if (!Cls->IsChildOf(AActor::StaticClass()))
        {
            return false;
        }

		FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        FActorSpawnParameters Params;
        Params.ObjectFlags = RF_Transient;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* A = World->SpawnActor<AActor>(Cls, FTransform::Identity, Params);
        if (!A)
        {
            return false;
        }

        // Center actor
        const FBox Box = A->GetComponentsBoundingBox(true);
        const FVector Center = Box.GetCenter();
        A->SetActorLocation(-Center);
        
        // Rotate actor 90° around Z-axis
        FRotator ActorRotation = A->GetActorRotation();
        ActorRotation.Yaw += 90.f;  // Add 90° rotation towards camera
        A->SetActorRotation(ActorRotation);

        const float Radius = Box.GetExtent().Size();
        const float BlueprintPadding = FMath::Max(1.05f, kDistancePadding * 0.75f);
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, BlueprintPadding);
        const FVector ViewDir = ChooseStableViewDirFromBoxExtent(Box.GetExtent());
        return CapturePreviewSceneToWebPBytes(Scene, FVector::ZeroVector, OutDistance, kDefaultFov, Resolution, OutWebP, ViewDir);
    }

    static bool CaptureBlueprintMultiFrame(UBlueprint* BP, int32 Resolution, TArray<FZipEntry>& OutFrames, float& OutDistance)
    {
        OutFrames.Reset();
        if (!BP || !BP->GeneratedClass)
        {
            return false;
        }

        UClass* Cls = BP->GeneratedClass;
        if (!Cls->IsChildOf(AActor::StaticClass()))
        {
            return false;
        }

        FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        FActorSpawnParameters Params;
        Params.ObjectFlags = RF_Transient;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* A = World->SpawnActor<AActor>(Cls, FTransform::Identity, Params);
        if (!A)
        {
            return false;
        }

        const FBox Box = A->GetComponentsBoundingBox(true);
        const FVector Center = Box.GetCenter();
        A->SetActorLocation(-Center);

        const FRotator ActorRotation = A->GetActorRotation();
        A->SetActorRotation(ActorRotation + FRotator(0.f, 90.f, 0.f));

        const float Radius = Box.GetExtent().Size();
        const float BlueprintPadding = FMath::Max(1.05f, kDistancePadding * 0.75f);
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, BlueprintPadding);
        const FVector ViewDir = ChooseStableViewDirFromBoxExtent(Box.GetExtent());

        const int32 FramesToKeep = GetBlueprintFrameCount();
        const int32 FramesToDiscard = GetCapture360DiscardCount();
        const int32 FramesTotal = FramesToKeep + FramesToDiscard;
        OutFrames.Reserve(FramesToKeep);
        for (int32 i = 0; i < FramesTotal; ++i)
        {
            if (i > 0)
            {
                World->Tick(LEVELTICK_All, kCapture360FrameInterval);
                A->MarkComponentsRenderStateDirty();
                FlushRenderingCommands();
                FPlatformProcess::Sleep(kCapture360FrameInterval);
            }

            float CameraYaw = 0.0f;
            if (i >= FramesToDiscard)
            {
                const int32 FrameIndex = i - FramesToDiscard;
                CameraYaw = (360.0f / (float)FramesToKeep) * FrameIndex;
            }

            TArray<uint8> WebP;
            if (CapturePreviewSceneToWebPBytes(Scene, FVector::ZeroVector, OutDistance, kDefaultFov, Resolution, WebP, ViewDir, CameraYaw))
            {
                if (i >= FramesToDiscard)
                {
                    FZipEntry Frame;
                    Frame.NameInZip = FString::Printf(TEXT("%d.webp"), OutFrames.Num());
                    Frame.Data = MoveTemp(WebP);
                    OutFrames.Add(MoveTemp(Frame));
                }
            }
        }

        return OutFrames.Num() > 0;
    }

#if ASSETSNAPSHOT_WITH_NIAGARA
    static bool CaptureNiagara(UNiagaraSystem* Sys, int32 Resolution, TArray<uint8>& OutWebP, float& OutDistance)
    {
        if (!Sys)
        {
            return false;
        }

		FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        AActor* A = World->SpawnActor<AActor>();
        UNiagaraComponent* Comp = NewObject<UNiagaraComponent>(A);
        Comp->SetAsset(Sys);
        Comp->SetAutoActivate(true);
        Comp->RegisterComponentWithWorld(World);
        A->SetRootComponent(Comp);
        A->SetActorLocation(FVector::ZeroVector);
        
        // Rotate actor 90° around Z-axis
        FRotator ActorRotation = A->GetActorRotation();
        ActorRotation.Yaw += 90.f;  // Add 90° rotation towards camera
        A->SetActorRotation(ActorRotation);

        // Let it simulate a bit
        for (int32 i = 0; i < 30; ++i)
        {
            const float Dt = 1.f / 60.f;
            World->Tick(LEVELTICK_All, Dt);
            IStreamingManager::Get().Tick(Dt);
        }
        FlushRenderingCommands();

        ForceComponentTexturesResident(Comp);

        const FBoxSphereBounds B = Comp->Bounds;
        const float Radius = FMath::Max(100.f, B.SphereRadius);
        OutDistance = ComputeCameraDistanceFromBounds(Radius, kDefaultFov, 1.35f);
        const FVector ViewDir = FVector(0.f, -1.f, 0.05f).GetSafeNormal();  // Y-axis
        return CapturePreviewSceneToWebPBytes(Scene, FVector::ZeroVector, OutDistance, kDefaultFov, Resolution, OutWebP, ViewDir);
    }
#endif

    static bool CaptureAnimSequence(UAnimSequence* Anim, int32 Resolution, TArray<FZipEntry>& OutFrames, float& OutDistance, float& OutAnimLen)
    {
        if (!Anim)
        {
            return false;
        }

        OutAnimLen = Anim->GetPlayLength();
        const int32 FrameCount = FMath::Clamp(GetAnimFrameCount(), 1, 32);

        USkeletalMesh* PreviewMesh = nullptr;
#if WITH_EDITOR
        PreviewMesh = Anim->GetPreviewMesh();
#endif
        if (!PreviewMesh && Anim->GetSkeleton())
        {
#if WITH_EDITOR
            PreviewMesh = Anim->GetSkeleton()->GetPreviewMesh();
#endif
        }
        if (!PreviewMesh)
        {
            return false;
        }

		FPreviewScene Scene{ FPreviewScene::ConstructionValues() };
        SetupDefaultLights(Scene);

        UWorld* World = Scene.GetWorld();
        if (!World)
        {
            return false;
        }

        USkeletalMeshComponent* Comp = NewObject<USkeletalMeshComponent>(GetTransientPackage());
        Comp->SetSkeletalMesh(PreviewMesh);
        Comp->SetMobility(EComponentMobility::Movable);
        Comp->RegisterComponentWithWorld(World);
        Scene.AddComponent(Comp, FTransform::Identity);
        Comp->SetForcedLOD(1);

        Comp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
        Comp->SetAnimation(Anim);
        Comp->Stop();

        // Ensure materials/textures are resident before the first capture
        ForceComponentTexturesResident(Comp);

        Comp->UpdateBounds();
        const FVector CenterOffset = -Comp->Bounds.Origin;
        const FVector ViewDir = FVector(1.f, 0.f, 0.02f).GetSafeNormal();
        Comp->SetWorldLocation(CenterOffset);
        
        // Rotate mesh: base rotation + 90° around Z-axis
        FRotator BaseRotation = FRotationMatrix::MakeFromX(-ViewDir).Rotator();
        BaseRotation.Yaw += 90.f;  // Add 90° rotation towards camera
        Comp->SetWorldRotation(BaseRotation);
        Comp->UpdateBounds();
        OutDistance = ComputeCameraDistanceFromBounds(Comp->Bounds.SphereRadius, kDefaultFov, kDistancePadding);

        // Pre-roll to let streaming/shaders settle before the first frame
        WarmupWorld(World, kWarmupSeconds);
        ForceComponentTexturesResident(Comp);

        OutFrames.Reset();
        OutFrames.Reserve(FrameCount);

        for (int32 i = 0; i < FrameCount; ++i)
        {
            const float Alpha = (FrameCount <= 1) ? 0.f : (float)i / (float)(FrameCount - 1);
            const float T = OutAnimLen * Alpha;

            if (UAnimSingleNodeInstance* Inst = Cast<UAnimSingleNodeInstance>(Comp->GetAnimInstance()))
            {
                Inst->SetPlaying(false);
                Inst->SetPosition(T, false);
            }
            Comp->TickAnimation(0.f, false);
            Comp->RefreshBoneTransforms();
            Comp->RefreshFollowerComponents();
            Comp->UpdateComponentToWorld();
            Comp->FinalizeBoneTransform();
            Comp->MarkRenderDynamicDataDirty();

            World->Tick(LEVELTICK_All, 1.f / 30.f);
            FlushRenderingCommands();

            TArray<uint8> WebP;
            if (!CapturePreviewSceneToWebPBytes(Scene, Comp->Bounds.Origin, OutDistance, kDefaultFov, Resolution, WebP, ViewDir))
            {
                continue;
            }

            FZipEntry Frame;
            Frame.NameInZip = FString::Printf(TEXT("%d.webp"), i);
            Frame.Data = MoveTemp(WebP);
            OutFrames.Add(MoveTemp(Frame));
        }

        return OutFrames.Num() > 0;
    }

    static TSharedPtr<FJsonObject> MeshStatsToJson(const FMeshStats& S)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("triangles"), (double)S.Triangles);
        J->SetNumberField(TEXT("polygons"), (double)S.Triangles);
        J->SetNumberField(TEXT("vertices"), (double)S.Vertices);
        J->SetNumberField(TEXT("lods"), (double)S.LODs);
        J->SetBoolField(TEXT("nanite_enabled"), S.bNaniteEnabled);
        J->SetStringField(TEXT("collision_complexity"), S.CollisionComplexity);

        TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
        Size->SetNumberField(TEXT("x"), S.ApproxSize.X);
        Size->SetNumberField(TEXT("y"), S.ApproxSize.Y);
        Size->SetNumberField(TEXT("z"), S.ApproxSize.Z);
        J->SetObjectField(TEXT("approx_size_cm"), Size);
        J->SetNumberField(TEXT("approx_size_max_cm"), (double)FMath::Max3(S.ApproxSize.X, S.ApproxSize.Y, S.ApproxSize.Z));
        return J;
    }

    static FString SerializeJson(const TSharedRef<FJsonObject>& Root)
    {
        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Root, Writer);
        return Out;
    }
}

FString UAssetSnapshotBPLibrary::GetDefaultExportRoot()
{
    return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("export"));
}

int32 UAssetSnapshotBPLibrary::ExportPathBuilds(const FString& InGamePath, const FString& InTypeFilter, const FString& InExcludeTypeFilter)
{
    ++GAssetSnapshotExportBatchId;
    GAssetSnapshotServerBatchId = GAssetSnapshotExportBatchId;
    GAssetSnapshotServerChecked = false;
    GAssetSnapshotServerAvailable = true;
    GAssetSnapshotServerWarned = false;
    GAssetSnapshotServerSkipKnown = false;
    GAssetSnapshotServerSkipEnabled = true;
    FString Path = InGamePath;
    Path.TrimStartAndEndInline();

    if (Path.IsEmpty())
    {
        UE_LOG(LogAssetSnapshot, Error, TEXT("ExportPathBuilds: empty path"));
        return 0;
    }

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARM.Get();

    TArray<FAssetData> Assets;

    if (Path.Contains(TEXT(".")))
    {
        const FSoftObjectPath SOP(Path);
		const FAssetData AD = AR.GetAssetByObjectPath(SOP);
        if (AD.IsValid())
        {
            Assets.Add(AD);
        }
    }

    if (Assets.Num() == 0)
    {
        // Directory path?
        if (Path.StartsWith(TEXT("/Game")))
        {
            AR.GetAssetsByPath(FName(*Path), Assets, true);
        }
    }

    if (Assets.Num() == 0)
    {
        // Maybe a package path without .ObjectName
        if (Path.StartsWith(TEXT("/Game")) && !Path.Contains(TEXT(".")))
        {
            const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
            const FString ObjPath = Path + TEXT(".") + AssetName;
            const FSoftObjectPath SOP(ObjPath);
			const FAssetData AD = AR.GetAssetByObjectPath(SOP);
            if (AD.IsValid())
            {
                Assets.Add(AD);
            }
        }
    }

    if (Assets.Num() == 0)
    {
        UE_LOG(LogAssetSnapshot, Error, TEXT("No assets found for: %s"), *Path);
        return 0;
    }

    const TSet<FName> AllowedClasses = {
        TEXT("StaticMesh"),
        TEXT("SkeletalMesh"),
        TEXT("Blueprint"),
        TEXT("NiagaraSystem"),
        TEXT("AnimSequence"),
        TEXT("Material"),
        TEXT("MaterialInstance"),
        TEXT("MaterialInstanceConstant")
    };

    auto AddClassesForToken = [&AllowedClasses](const FString& Token, TSet<FName>& OutSet)
    {
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("animation") || Lower == TEXT("anim") || Lower == TEXT("animsequence"))
        {
            OutSet.Add(TEXT("AnimSequence"));
        }
        else if (Lower == TEXT("mesh") || Lower == TEXT("meshes"))
        {
            OutSet.Add(TEXT("StaticMesh"));
            OutSet.Add(TEXT("SkeletalMesh"));
        }
        else if (Lower == TEXT("staticmesh"))
        {
            OutSet.Add(TEXT("StaticMesh"));
        }
        else if (Lower == TEXT("skeletalmesh"))
        {
            OutSet.Add(TEXT("SkeletalMesh"));
        }
        else if (Lower == TEXT("material") || Lower == TEXT("materials") || Lower == TEXT("mat"))
        {
            OutSet.Add(TEXT("Material"));
            OutSet.Add(TEXT("MaterialInstance"));
            OutSet.Add(TEXT("MaterialInstanceConstant"));
        }
        else if (Lower == TEXT("materialinstance") || Lower == TEXT("materialinstanceconstant"))
        {
            OutSet.Add(TEXT("MaterialInstance"));
            OutSet.Add(TEXT("MaterialInstanceConstant"));
        }
        else if (Lower == TEXT("blueprint") || Lower == TEXT("bp"))
        {
            OutSet.Add(TEXT("Blueprint"));
        }
        else if (Lower == TEXT("niagara") || Lower == TEXT("niagarasystem"))
        {
            OutSet.Add(TEXT("NiagaraSystem"));
        }
        else
        {
            // Allow explicit class names like "StaticMesh"
            OutSet.Add(FName(*Token));
        }

    };

    FString FilterRaw = InTypeFilter;
    FString ExcludeRaw = InExcludeTypeFilter;
    const bool bHasCliInclude = !FilterRaw.TrimStartAndEnd().IsEmpty();
    FString ServerIncludeRaw;
    FString ServerExcludeRaw;
    bool bGotServerFilters = false;
    bool bUsedCache = false;
    if (const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>())
    {
        if (!Settings->ImportBaseUrl.IsEmpty())
        {
            const bool bOk = AssetSnapshot::GetServerExportFilters(Settings->ImportBaseUrl, ServerIncludeRaw, ServerExcludeRaw);
            GAssetSnapshotServerChecked = true;
            GAssetSnapshotServerAvailable = bOk;
            if (bOk)
            {
                bGotServerFilters = true;
                if (!ServerIncludeRaw.IsEmpty() && !bHasCliInclude)
                {
                    FilterRaw = ServerIncludeRaw;
                }

                if (!ServerExcludeRaw.IsEmpty())
                {
                    if (ExcludeRaw.IsEmpty())
                    {
                        ExcludeRaw = ServerExcludeRaw;
                    }
                    else
                    {
                        ExcludeRaw = ExcludeRaw + TEXT(",") + ServerExcludeRaw;
                    }
                }
            }
            else if (!GAssetSnapshotServerWarned)
            {
                UE_LOG(LogAssetSnapshot, Warning, TEXT("Export filters: server settings unavailable (baseUrl='%s')"), *Settings->ImportBaseUrl);
                GAssetSnapshotServerWarned = true;
            }
        }
    }
    UE_LOG(LogAssetSnapshot, Log, TEXT("Export filters: include='%s' exclude='%s' (server=%s)"),
        *FilterRaw,
        *ExcludeRaw,
        bGotServerFilters ? TEXT("yes") : (bUsedCache ? TEXT("cache") : TEXT("no")));
    FilterRaw.TrimStartAndEndInline();
    auto ParseTokens = [](const FString& Raw, TArray<FString>& OutTokens)
    {
        FString Temp = Raw;
        Temp.ReplaceInline(TEXT(";"), TEXT(","));
        Temp.ReplaceInline(TEXT("|"), TEXT(","));
        Temp.ReplaceInline(TEXT(" "), TEXT(","));
        Temp.ParseIntoArray(OutTokens, TEXT(","), true);
    };

    TSet<FName> FilterClasses;
    const bool bHasFilter = !FilterRaw.IsEmpty();
    if (bHasFilter)
    {
        TArray<FString> Tokens;
        ParseTokens(FilterRaw, Tokens);
        for (FString T : Tokens)
        {
            T.TrimStartAndEndInline();
            if (T.IsEmpty())
            {
                continue;
            }
            AddClassesForToken(T, FilterClasses);
        }

        if (bHasCliInclude && !ServerIncludeRaw.IsEmpty())
        {
            TSet<FName> ServerIncludeClasses;
            TArray<FString> ServerTokens;
            ParseTokens(ServerIncludeRaw, ServerTokens);
            for (FString T : ServerTokens)
            {
                T.TrimStartAndEndInline();
                if (T.IsEmpty())
                {
                    continue;
                }
                AddClassesForToken(T, ServerIncludeClasses);
            }
            if (ServerIncludeClasses.Num() > 0)
            {
                FilterClasses = FilterClasses.Intersect(ServerIncludeClasses);
            }
        }

        // Keep only classes we can export.
        FilterClasses = FilterClasses.Intersect(AllowedClasses);
        if (FilterClasses.Num() == 0)
        {
            UE_LOG(LogAssetSnapshot, Warning, TEXT("Type filter did not match any exportable classes: %s"), *FilterRaw);
            return 0;
        }
    }

    ExcludeRaw.TrimStartAndEndInline();
    TSet<FName> ExcludeClasses;
    const bool bHasExclude = !ExcludeRaw.IsEmpty();
    if (bHasExclude)
    {
        TArray<FString> Tokens;
        ParseTokens(ExcludeRaw, Tokens);
        UE_LOG(LogAssetSnapshot, Log, TEXT("Exclude tokens: %s"), *FString::Join(Tokens, TEXT("|")));
        for (FString T : Tokens)
        {
            T.TrimStartAndEndInline();
            if (T.IsEmpty())
            {
                continue;
            }
            AddClassesForToken(T, ExcludeClasses);
        }
        ExcludeClasses = ExcludeClasses.Intersect(AllowedClasses);
        if (bHasFilter)
        {
            ExcludeClasses = ExcludeClasses.Difference(FilterClasses);
        }
        TArray<FString> ExcludeNames;
        ExcludeNames.Reserve(ExcludeClasses.Num());
        for (const FName& Name : ExcludeClasses)
        {
            ExcludeNames.Add(Name.ToString());
        }
        ExcludeNames.Sort();
        UE_LOG(LogAssetSnapshot, Log, TEXT("Export exclude classes: %s"), *FString::Join(ExcludeNames, TEXT(",")));
    }

    TArray<FAssetData> Filtered;
    Filtered.Reserve(Assets.Num());
    int32 LoggedExcludeChecks = 0;
    for (const FAssetData& AD : Assets)
    {
        const FName ClassName = AD.AssetClassPath.GetAssetName();
        if (bHasExclude
            && (ClassName == TEXT("Material") || ClassName == TEXT("MaterialInstance") || ClassName == TEXT("MaterialInstanceConstant"))
            && LoggedExcludeChecks < 10)
        {
            UE_LOG(LogAssetSnapshot, Log, TEXT("Exclude check: class=%s excluded=%s raw='%s'"),
                *ClassName.ToString(),
                ExcludeClasses.Contains(ClassName) ? TEXT("yes") : TEXT("no"),
                *ExcludeRaw);
            ++LoggedExcludeChecks;
        }
        if (AllowedClasses.Contains(ClassName)
            && (!bHasFilter || FilterClasses.Contains(ClassName))
            && (!bHasExclude || !ExcludeClasses.Contains(ClassName)))
        {
            Filtered.Add(AD);
        }
    }

    if (Filtered.Num() == 0)
    {
        UE_LOG(LogAssetSnapshot, Warning, TEXT("No matching asset types to export for: %s"), *Path);
        return 0;
    }

    auto GetSortKey = [](const FAssetData& AD)
    {
        const FName ClassName = AD.AssetClassPath.GetAssetName();
        if (ClassName == TEXT("MaterialInstance") || ClassName == TEXT("MaterialInstanceConstant"))
        {
            return 0;
        }
        if (ClassName == TEXT("Material"))
        {
            return 1;
        }
        if (ClassName == TEXT("AnimSequence"))
        {
            return 2;
        }
        if (ClassName == TEXT("StaticMesh") || ClassName == TEXT("SkeletalMesh"))
        {
            return 3;
        }
        if (ClassName == TEXT("Blueprint"))
        {
            return 4;
        }
        if (ClassName == TEXT("NiagaraSystem"))
        {
            return 5;
        }
        return 5;
    };

    Filtered.Sort([&](const FAssetData& A, const FAssetData& B)
    {
        const int32 KeyA = GetSortKey(A);
        const int32 KeyB = GetSortKey(B);
        if (KeyA != KeyB)
        {
            return KeyA < KeyB;
        }
        return A.ObjectPath.ToString() < B.ObjectPath.ToString();
    });

    bool bHasMaterials = false;
    for (const FAssetData& AD : Filtered)
    {
        const FName ClassName = AD.AssetClassPath.GetAssetName();
        if (ClassName == TEXT("Material") || ClassName == TEXT("MaterialInstance") || ClassName == TEXT("MaterialInstanceConstant"))
        {
            bHasMaterials = true;
            break;
        }
    }

    AssetSnapshot::FMaterialCaptureContext MaterialCtx;
    if (bHasMaterials)
    {
        if (AssetSnapshot::InitMaterialCaptureContext(MaterialCtx))
        {
            AssetSnapshot::GMaterialCaptureContext = &MaterialCtx;
        }
        else
        {
            UE_LOG(LogAssetSnapshot, Warning, TEXT("Failed to initialize shared material capture context; falling back to per-asset scenes."));
        }
    }

    int32 Exported = 0;
    const int32 Total = Filtered.Num();

    GAssetSnapshotExportTotal = Total;
    for (int32 i = 0; i < Total; ++i)
    {
        GAssetSnapshotExportCurrent = i + 1;
        const int32 Pct = FMath::RoundToInt(((float)(i + 1) / (float)Total) * 100.0f);
        UE_LOG(LogAssetSnapshot, Log, TEXT("[%d/%d] (%d%%) Exporting %s"), i + 1, Total, Pct, *Filtered[i].ObjectPath.ToString());

        UObject* Obj = Filtered[i].GetAsset();
        if (!Obj)
        {
            continue;
        }

        if (ExportAssetBuild(Obj))
        {
            ++Exported;
        }

#if WITH_EDITOR
        // Keep RAM in check when batch-exporting
        if ((i % 10) == 9)
        {
            CollectGarbage(RF_NoFlags);
        }
#endif
    }

    AssetSnapshot::GMaterialCaptureContext = nullptr;
    GAssetSnapshotExportTotal = 0;
    GAssetSnapshotExportCurrent = 0;
    UE_LOG(LogAssetSnapshot, Log, TEXT("Export done. Exported: %d/%d"), Exported, Total);
    return Exported;
}

bool UAssetSnapshotBPLibrary::ExportAssetBuild(UObject* Asset)
{
    if (!Asset)
    {
        return false;
    }

    const FString PackageName = Asset->GetOutermost()->GetName();
    if (!PackageName.StartsWith(TEXT("/Game/")))
    {
        UE_LOG(LogAssetSnapshot, Warning, TEXT("Skipping non-/Game asset: %s"), *PackageName);
        return false;
    }

    // Dependencies
    TArray<FString> DepPackages;
    AssetSnapshot::GatherGameDependenciesPackages(PackageName, DepPackages);

    // Files on disk
    TSet<FString> SeenRel;
    TArray<FString> FilesRel;
    TArray<FString> FilesAbs;
    int64 DiskBytesTotal = 0;

    for (const FString& Pkg : DepPackages)
    {
        AssetSnapshot::GatherFilesOnDiskForPackage(Pkg, SeenRel, FilesRel, FilesAbs, DiskBytesTotal);
    }

    // Sort deterministically
    TArray<int32> SortIdx;
    SortIdx.Reserve(FilesRel.Num());
    for (int32 i = 0; i < FilesRel.Num(); ++i) SortIdx.Add(i);
    SortIdx.Sort([&](int32 A, int32 B) { return FilesRel[A] < FilesRel[B]; });

    TArray<FString> FilesRelSorted;
    TArray<FString> FilesAbsSorted;
    FilesRelSorted.Reserve(SortIdx.Num());
    FilesAbsSorted.Reserve(SortIdx.Num());
    for (int32 I : SortIdx)
    {
        FilesRelSorted.Add(FilesRel[I]);
        FilesAbsSorted.Add(FilesAbs[I]);
    }

    auto NormalizeZipRel = [](const FString& InPath)
    {
        FString Clean = InPath;
        Clean.TrimStartAndEndInline();
        Clean.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Clean.StartsWith(TEXT("/")))
        {
            Clean.RightChopInline(1);
        }
        if (Clean.StartsWith(TEXT("Content/")))
        {
            Clean.RightChopInline(8);
        }
        return Clean;
    };

    TSet<FString> RootFolders;
    RootFolders.Reserve(FilesRelSorted.Num());
    for (const FString& Rel : FilesRelSorted)
    {
        const FString Clean = NormalizeZipRel(Rel);
        FString Top = Clean;
        Clean.Split(TEXT("/"), &Top, nullptr);
        if (!Top.IsEmpty())
        {
            RootFolders.Add(Top);
        }
    }

    // Hashes
    FString MainFileAbs;
    AssetSnapshot::PackageToMainFileAbs(PackageName, MainFileAbs);

    FString HashMain;
    FString HashMainSha256;
    if (!AssetSnapshot::Blake3HashFile(MainFileAbs, HashMain))
    {
        UE_LOG(LogAssetSnapshot, Error, TEXT("Failed to hash main file: %s"), *MainFileAbs);
        return false;
    }
    if (!AssetSnapshot::Sha256HashFile(MainFileAbs, HashMainSha256))
    {
        HashMainSha256 = TEXT("");
    }

    FString HashFull;
    AssetSnapshot::Blake3HashFiles(FilesAbsSorted, FilesRelSorted, HashFull);

    if (const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>())
    {
        static int32 LastBatchId = -1;
        static bool CachedUseServerCheck = false;
        static bool CachedServerSettingKnown = false;

        if (LastBatchId != GAssetSnapshotExportBatchId)
        {
            const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings->ImportBaseUrl);
            CachedServerSettingKnown = Server.bAvailable;
            CachedUseServerCheck = Server.bAvailable ? Server.bSkipExportIfOnServer : false;
            LastBatchId = GAssetSnapshotExportBatchId;

            if (!Server.bAvailable && !Settings->ImportBaseUrl.IsEmpty() && !GAssetSnapshotServerWarned)
            {
                UE_LOG(LogAssetSnapshot, Warning, TEXT("Export server check disabled: server settings unavailable (baseUrl='%s')"), *Settings->ImportBaseUrl);
                GAssetSnapshotServerWarned = true;
            }
            UE_LOG(LogAssetSnapshot, Log, TEXT("Export server check: use=%s (serverSetting=%s baseUrl='%s')"),
                CachedUseServerCheck ? TEXT("true") : TEXT("false"),
                Server.bAvailable ? (Server.bSkipExportIfOnServer ? TEXT("true") : TEXT("false")) : TEXT("unavailable"),
                *Settings->ImportBaseUrl);
        }

        if (CachedUseServerCheck && !Settings->ImportBaseUrl.IsEmpty())
        {
            bool bExists = false;
            const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings->ImportBaseUrl);
            UE_LOG(LogAssetSnapshot, Log, TEXT("Checking server for hash %s"), *HashMain);
            if (AssetSnapshot::CheckServerHasHash(Settings->ImportBaseUrl, Server.ExportCheckPathTemplate, HashMain, bExists) && bExists)
            {
                UE_LOG(LogAssetSnapshot, Log, TEXT("Server already has hash %s, skipping export."), *HashMain);
                return false;
            }
        }
        else if (!Settings->ImportBaseUrl.IsEmpty())
        {
            bool bExists = false;
            UE_LOG(LogAssetSnapshot, Log, TEXT("Checking server (fallback) for hash %s"), *HashMain);
            if (AssetSnapshot::CheckServerHasHash(Settings->ImportBaseUrl, TEXT("/assets/exists?hash={hash}&hash_type=blake3"), HashMain, bExists) && bExists)
            {
                UE_LOG(LogAssetSnapshot, Log, TEXT("Server already has hash %s, skipping export."), *HashMain);
                return false;
            }
        }
    }

    // Export target path (skip if already exported)
    const auto GetExportSubdirFromGamePackage = [](const FString& GamePackageName) -> FString
    {
        // "/Game/<Top>/..." -> "<Top>"
        FString Tail = GamePackageName;
        Tail.RemoveFromStart(TEXT("/Game/"));

        TArray<FString> Parts;
        Tail.ParseIntoArray(Parts, TEXT("/"), true);
        if (Parts.Num() == 0)
        {
            return Tail;
        }

        // If you keep vendor namespaces like /Game/byHans1/<Pack>/..., export under <Pack>.
        if (Parts[0].Equals(TEXT("byHans1"), ESearchCase::IgnoreCase) && Parts.Num() > 1)
        {
            return Parts[1];
        }

        return Parts[0];
    };

    const FString ExportSubdir = GetExportSubdirFromGamePackage(PackageName);
    const FString ExportRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("export") / ExportSubdir);
    const FString ZipPath = ExportRoot / (HashMain + TEXT(".zip"));

    if (IFileManager::Get().FileExists(*ZipPath))
    {
        const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings ? Settings->ImportBaseUrl : FString());
        const bool bOverwrite = Server.bOverwriteExportZips;
        if (!bOverwrite)
        {
            UE_LOG(LogAssetSnapshot, Log, TEXT("Zip already exists, skipping: %s"), *ZipPath);
            return false;
        }
        IFileManager::Get().Delete(*ZipPath, false, true, true);
    }

    // Capture preview(s)
    int32 Resolution = AssetSnapshot::kDefaultResolution;

    TArray<AssetSnapshot::FZipEntry> ZipEntries;
    ZipEntries.Reserve(16 + FilesAbsSorted.Num());

    // Keep zips minimal; do not pack additional files outside the primary root.

    float CamDistance = 0.f;
    FString AssetType = Asset->GetClass()->GetName();

    // Skip Texture2D assets
    if (Cast<UTexture2D>(Asset))
    {
        UE_LOG(LogAssetSnapshot, Log, TEXT("Skipping Texture2D: %s"), *Asset->GetPathName());
        return false;
    }

    // Stats + capture
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("hash_main_blake3"), HashMain);
    Root->SetStringField(TEXT("hash_main_sha256"), HashMainSha256);
    Root->SetStringField(TEXT("hash_full_blake3"), HashFull);
    Root->SetStringField(TEXT("package"), PackageName);
    FString VendorName;
    {
        FString Tail = PackageName;
        Tail.RemoveFromStart(TEXT("/Game/"));
        TArray<FString> Parts;
        Tail.ParseIntoArray(Parts, TEXT("/"), true);
        if (Parts.Num() > 0)
        {
            VendorName = Parts[0];
        }
    }
    Root->SetStringField(TEXT("vendor"), VendorName);
    const FString SourcePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    Root->SetStringField(TEXT("source_path"), SourcePath);
    if (!VendorName.IsEmpty())
    {
        Root->SetStringField(TEXT("source_folder"), VendorName);
    }
    Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
    Root->SetStringField(TEXT("class"), AssetType);
    Root->SetStringField(TEXT("exported_at_utc"), FDateTime::UtcNow().ToIso8601());
    if (RootFolders.Num() > 1)
    {
        TArray<FString> RootsArray = RootFolders.Array();
        RootsArray.Sort();
        Root->SetBoolField(TEXT("path_warning"), true);
        TArray<TSharedPtr<FJsonValue>> RootValues;
        RootValues.Reserve(RootsArray.Num());
        for (const FString& RootName : RootsArray)
        {
            RootValues.Add(MakeShared<FJsonValueString>(RootName));
        }
        Root->SetArrayField(TEXT("path_roots"), RootValues);
        UE_LOG(LogAssetSnapshot, Warning, TEXT("Export: asset spans multiple roots: %s"), *FString::Join(RootsArray, TEXT(", ")));
    }

    // files on disk
    TArray<TSharedPtr<FJsonValue>> FilesJson;
    FilesJson.Reserve(FilesRelSorted.Num());
    for (const FString& P : FilesRelSorted)
    {
        const FString Clean = NormalizeZipRel(P);
        FilesJson.Add(MakeShared<FJsonValueString>(Clean));
    }
    Root->SetArrayField(TEXT("files_on_disk"), FilesJson);
    Root->SetNumberField(TEXT("disk_bytes_total"), (double)DiskBytesTotal);

    TArray<TSharedPtr<FJsonValue>> PreviewFiles;

    bool bCaptured = false;
    bool bNoPic = false;
    bool bLowQuality = false;

    if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    {
        const AssetSnapshot::FMeshStats Stats = AssetSnapshot::GetStaticMeshStats(SM);
        Root->SetObjectField(TEXT("mesh"), AssetSnapshot::MeshStatsToJson(Stats));

        // Multi-frame for animated materials on mesh
        TArray<AssetSnapshot::FZipEntry> Frames;
        bCaptured = AssetSnapshot::CaptureStaticMeshMultiFrame(SM, Resolution, Frames, CamDistance);
        if (bCaptured)
        {
            for (AssetSnapshot::FZipEntry& F : Frames)
            {
                PreviewFiles.Add(MakeShared<FJsonValueString>(F.NameInZip));
                ZipEntries.Add(MoveTemp(F));
            }
        }
    }
    else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
    {
        const AssetSnapshot::FMeshStats Stats = AssetSnapshot::GetSkeletalMeshStats(SK);
        Root->SetObjectField(TEXT("mesh"), AssetSnapshot::MeshStatsToJson(Stats));

        // Multi-frame for animated materials on mesh
        TArray<AssetSnapshot::FZipEntry> Frames;
        bCaptured = AssetSnapshot::CaptureSkeletalMeshMultiFrame(SK, Resolution, Frames, CamDistance);
        if (bCaptured)
        {
            for (AssetSnapshot::FZipEntry& F : Frames)
            {
                PreviewFiles.Add(MakeShared<FJsonValueString>(F.NameInZip));
                ZipEntries.Add(MoveTemp(F));
            }
        }
    }
    else if (UMaterialInterface* Mat = Cast<UMaterialInterface>(Asset))
    {
        Resolution = AssetSnapshot::kTexturePreviewResolution;

        // Single multi-frame capture for animated materials.
        TArray<AssetSnapshot::FZipEntry> Frames;
        if (AssetSnapshot::GMaterialCaptureContext)
        {
            bCaptured = AssetSnapshot::CaptureMaterialOnSharedSphereMultiFrame(*AssetSnapshot::GMaterialCaptureContext, Mat, Resolution, Frames, CamDistance, bLowQuality);
        }
        else
        {
            bCaptured = AssetSnapshot::CaptureMaterialOnSphereMultiFrame(Mat, Resolution, Frames, CamDistance, bLowQuality);
        }
        if (bCaptured)
        {
            for (AssetSnapshot::FZipEntry& F : Frames)
            {
                PreviewFiles.Add(MakeShared<FJsonValueString>(F.NameInZip));
                ZipEntries.Add(MoveTemp(F));
            }
        }
    }
    else if (UBlueprint* BP = Cast<UBlueprint>(Asset))
    {
        Root->SetStringField(TEXT("class"), TEXT("Blueprint"));
        TArray<AssetSnapshot::FZipEntry> Frames;
        bCaptured = AssetSnapshot::CaptureBlueprintMultiFrame(BP, Resolution, Frames, CamDistance);
        if (bCaptured)
        {
            for (AssetSnapshot::FZipEntry& F : Frames)
            {
                PreviewFiles.Add(MakeShared<FJsonValueString>(F.NameInZip));
                ZipEntries.Add(MoveTemp(F));
            }
        }
        else
        {
            TArray<uint8> WebP;
            bCaptured = AssetSnapshot::CaptureBlueprint(BP, Resolution, WebP, CamDistance);
            if (bCaptured)
            {
                AssetSnapshot::FZipEntry E;
                E.NameInZip = TEXT("0.webp");
                E.Data = MoveTemp(WebP);
                PreviewFiles.Add(MakeShared<FJsonValueString>(E.NameInZip));
                ZipEntries.Add(MoveTemp(E));
            }
        }
    }
#if ASSETSNAPSHOT_WITH_NIAGARA
    else if (UNiagaraSystem* Sys = Cast<UNiagaraSystem>(Asset))
    {
        Root->SetStringField(TEXT("class"), TEXT("NiagaraSystem"));

        TArray<uint8> WebP;
        bCaptured = AssetSnapshot::CaptureNiagara(Sys, Resolution, WebP, CamDistance);
        if (bCaptured)
        {
            AssetSnapshot::FZipEntry E;
            E.NameInZip = TEXT("0.webp");
            E.Data = MoveTemp(WebP);
            PreviewFiles.Add(MakeShared<FJsonValueString>(E.NameInZip));
            ZipEntries.Add(MoveTemp(E));
        }
    }
#endif
    else if (UAnimSequence* Anim = Cast<UAnimSequence>(Asset))
    {
        Root->SetStringField(TEXT("class"), TEXT("AnimSequence"));
        float AnimLen = 0.f;
        float AnimLenAttempt = 0.f;
        TArray<AssetSnapshot::FZipEntry> Frames;
        bCaptured = AssetSnapshot::CaptureAnimSequence(Anim, Resolution, Frames, CamDistance, AnimLenAttempt);
        AnimLen = AnimLenAttempt;
        if (bCaptured)
        {
            // Frame metadata for downstream vision / analysis
            Root->SetNumberField(TEXT("frame_count"), (double)Frames.Num());
            TArray<TSharedPtr<FJsonValue>> FrameMeta;

            const int32 N = Frames.Num();
            for (AssetSnapshot::FZipEntry& F : Frames)
            {
                const int32 FrameIdx = FrameMeta.Num();
                const double T = (N <= 1 || AnimLen <= 0.f) ? 0.0 : (double)FrameIdx / (double)(N - 1) * (double)AnimLen;
                TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
                FrameObj->SetNumberField(TEXT("index"), (double)FrameIdx);
                FrameObj->SetNumberField(TEXT("time_seconds"), T);
                FrameObj->SetStringField(TEXT("file"), F.NameInZip);
                FrameMeta.Add(MakeShared<FJsonValueObject>(FrameObj));

                PreviewFiles.Add(MakeShared<FJsonValueString>(F.NameInZip));
                ZipEntries.Add(MoveTemp(F));
            }

            Root->SetArrayField(TEXT("frames"), FrameMeta);
        }
        Root->SetNumberField(TEXT("animation_length_seconds"), (double)AnimLen);
    }
    else
    {
        UE_LOG(LogAssetSnapshot, Warning, TEXT("Unsupported asset type for capture: %s (%s)"), *Asset->GetPathName(), *AssetType);
    }

    if (!bCaptured || PreviewFiles.Num() == 0)
    {
        bNoPic = true;
        AddBlackPreview(PreviewFiles, ZipEntries, AssetSnapshot::kTexturePreviewResolution);
    }

    Root->SetArrayField(TEXT("preview_files"), PreviewFiles);
    Root->SetNumberField(TEXT("no_pic"), bNoPic ? 1.0 : 0.0);
    Root->SetNumberField(TEXT("low_quality"), bLowQuality ? 1.0 : 0.0);
    Root->SetNumberField(TEXT("capture_resolution"), (double)Resolution);
    Root->SetNumberField(TEXT("capture_fov"), (double)AssetSnapshot::kDefaultFov);
    Root->SetNumberField(TEXT("capture_distance"), (double)CamDistance);

    const FString MetaStr = AssetSnapshot::SerializeJson(Root);

    AssetSnapshot::FZipEntry Meta;
    Meta.NameInZip = TEXT("meta.json");
    FTCHARToUTF8 MetaUtf8(*MetaStr);
    Meta.Data.Append((uint8*)MetaUtf8.Get(), MetaUtf8.Length());

    ZipEntries.Insert(MoveTemp(Meta), 0);

    // Write zip
    const bool bZipOk = AssetSnapshot::WriteZipStore(ZipPath, ZipEntries);
    if (!bZipOk)
    {
        return false;
    }

    if (const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>())
    {
        const AssetSnapshot::FServerSettingsCache& Server = AssetSnapshot::GetServerSettingsCached(Settings->ImportBaseUrl);
        if (Server.bUploadAfterExport && !Settings->ImportBaseUrl.IsEmpty())
        {
            static FString CachedProjectPath;
            static int32 CachedProjectId = 0;
            const FString AssetName = Asset ? Asset->GetName() : TEXT("asset");

            FString ResolvePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
            if (Asset)
            {
                const FString AssetPackageName = Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString();
                const FString PackagePath = FPackageName::GetLongPackagePath(AssetPackageName);
                if (PackagePath.StartsWith(TEXT("/Game/")))
                {
                    const FString RelativePath = PackagePath.Mid(6);
                    FString TopFolder;
                    FString Remainder;
                    if (RelativePath.Split(TEXT("/"), &TopFolder, &Remainder))
                    {
                        if (!TopFolder.IsEmpty())
                        {
                            ResolvePath = FPaths::ConvertRelativePathToFull(
                                FPaths::Combine(FPaths::ProjectContentDir(), TopFolder));
                        }
                    }
                    else if (!RelativePath.IsEmpty())
                    {
                        ResolvePath = FPaths::ConvertRelativePathToFull(
                            FPaths::Combine(FPaths::ProjectContentDir(), RelativePath));
                    }
                }
            }

            if (CachedProjectPath != ResolvePath)
            {
                CachedProjectId = 0;
                AssetSnapshot::ResolveProjectIdFromServer(Settings->ImportBaseUrl, ResolvePath, CachedProjectId);
                CachedProjectPath = ResolvePath;
            }

                if (CachedProjectId > 0)
                {
                    const bool bUploaded = AssetSnapshot::UploadZipToServer(
                        Settings->ImportBaseUrl,
                        Server.ExportUploadPathTemplate,
                        ZipPath,
                        CachedProjectId);
                    if (!bUploaded)
                    {
                        UE_LOG(LogAssetSnapshot, Warning, TEXT("Export upload failed for %s"), *ZipPath);
                    }
                    else
                    {
                        AssetSnapshot::SendUploadEvent(Settings->ImportBaseUrl, AssetName);
                    }
                }
            else
            {
                UE_LOG(LogAssetSnapshot, Warning, TEXT("Export upload skipped: project id not resolved."));
            }
        }
    }

    UE_LOG(LogAssetSnapshot, Log, TEXT("Wrote: %s"), *ZipPath);
    return true;
}

bool UAssetSnapshotBPLibrary::ImportSnapshotZip(const FString& ZipPath, EAssetSnapshotImportMode Mode, FString& OutError)
{
    OutError.Reset();

    if (ZipPath.IsEmpty())
    {
        OutError = TEXT("ZipPath is empty.");
        return false;
    }

    const FString AbsZipPath = FPaths::ConvertRelativePathToFull(ZipPath);
    if (!IFileManager::Get().FileExists(*AbsZipPath))
    {
        OutError = FString::Printf(TEXT("Zip file not found: %s"), *AbsZipPath);
        return false;
    }

    const FString ContentRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    UE_LOG(
        LogAssetSnapshot,
        Log,
        TEXT("Import snapshot zip: %s -> %s (mode=%s)"),
        *AbsZipPath,
        *ContentRoot,
        Mode == EAssetSnapshotImportMode::OverrideExisting ? TEXT("override") : TEXT("skip"));
    return AssetSnapshot::ExtractZipStore(AbsZipPath, ContentRoot, Mode, OutError);
}

void UAssetSnapshotBPLibrary::DownloadAndImportSnapshot(const FString& SnapshotId, EAssetSnapshotImportMode Mode, const FAssetSnapshotImportResult& OnComplete)
{
    FAssetSnapshotImportResultNative Native;
    Native.BindLambda([OnComplete](bool bOk, const FString& Error)
    {
        OnComplete.ExecuteIfBound(bOk, Error);
    });
    DownloadAndImportSnapshotNative(SnapshotId, Mode, Native);
}

void UAssetSnapshotBPLibrary::DownloadAndImportSnapshotNative(const FString& SnapshotId, EAssetSnapshotImportMode Mode, const FAssetSnapshotImportResultNative& OnComplete)
{
    const UAssetSnapshotSettings* Settings = GetDefault<UAssetSnapshotSettings>();
    if (!Settings)
    {
        OnComplete.ExecuteIfBound(false, TEXT("AssetSnapshotSettings not available."));
        return;
    }

    if (Settings->ImportBaseUrl.IsEmpty())
    {
        OnComplete.ExecuteIfBound(false, TEXT("ImportBaseUrl is empty in AssetSnapshotSettings."));
        return;
    }

    if (SnapshotId.IsEmpty())
    {
        OnComplete.ExecuteIfBound(false, TEXT("SnapshotId is empty."));
        return;
    }

    const FString Url = AssetSnapshot::BuildSnapshotUrl(Settings->ImportBaseUrl, TEXT("/download/{id}.zip"), SnapshotId);
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(Url);
    Request->SetVerb(TEXT("GET"));

    Request->OnProcessRequestComplete().BindLambda(
        [SnapshotId, Mode, OnComplete](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
        {
            if (!bSucceeded || !Resp.IsValid())
            {
                OnComplete.ExecuteIfBound(false, TEXT("HTTP request failed."));
                return;
            }

            const int32 Code = Resp->GetResponseCode();
            if (Code != 200)
            {
                OnComplete.ExecuteIfBound(false, FString::Printf(TEXT("HTTP %d"), Code));
                return;
            }

            const TArray<uint8>& Content = Resp->GetContent();
            if (Content.Num() == 0)
            {
                OnComplete.ExecuteIfBound(false, TEXT("Empty response body."));
                return;
            }

            const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetSnapshotImports"));
            IFileManager::Get().MakeDirectory(*TempDir, true);
            const FString ZipPath = TempDir / (SnapshotId + TEXT(".zip"));

            if (!FFileHelper::SaveArrayToFile(Content, *ZipPath))
            {
                OnComplete.ExecuteIfBound(false, FString::Printf(TEXT("Failed to save zip: %s"), *ZipPath));
                return;
            }

            FString Error;
            const bool bOk = UAssetSnapshotBPLibrary::ImportSnapshotZip(ZipPath, Mode, Error);
            OnComplete.ExecuteIfBound(bOk, Error);
        });

    Request->ProcessRequest();
}
