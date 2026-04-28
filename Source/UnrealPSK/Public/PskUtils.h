#pragma once
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Paths.h"

class FPskUtils
{
public:
	static FString ResolveImportFolder(const UObject* FactoryParent, const FString& RootAssetName)
	{
		const FString ParentPath = FactoryParent ? FactoryParent->GetPathName() : FString();
		const FString ParentLeaf = FPaths::GetBaseFilename(ParentPath);
		return ParentLeaf.Equals(RootAssetName, ESearchCase::CaseSensitive)
			? FPaths::GetPath(ParentPath)
			: ParentPath;
	}

	static FString ResolveRootAssetPackagePath(const UObject* FactoryParent, const FString& RootAssetName)
	{
		const FString ParentPath = FactoryParent ? FactoryParent->GetPathName() : FString();
		const FString ParentLeaf = FPaths::GetBaseFilename(ParentPath);
		return ParentLeaf.Equals(RootAssetName, ESearchCase::CaseSensitive)
			? ParentPath
			: FPaths::Combine(ParentPath, RootAssetName);
	}

	template <typename T, typename = UObject>
	static T* LocalFindOrCreateInPackage(UClass* StaticClass, const FString& PackagePath, FString Filename, EObjectFlags Flags)
	{
		const auto Package = CreatePackage(*PackagePath);

		auto Asset = LoadObject<T>(Package, *Filename);
		if (!Asset)
		{
			Asset = NewObject<T>(Package, StaticClass, FName(Filename), Flags);
			Asset->PostEditChange();
			FAssetRegistryModule::AssetCreated(Asset);
			Asset->MarkPackageDirty();
		}

		return Asset;
	}

	template <typename T>
	static T* LocalCreateInPackage(UClass* StaticClass, const FString& PackagePath, FString Filename, EObjectFlags Flags)
	{
		const auto Package = CreatePackage(*PackagePath);

		auto Asset = NewObject<T>(Package, StaticClass, FName(Filename), Flags);
		return Asset;
	}
};
