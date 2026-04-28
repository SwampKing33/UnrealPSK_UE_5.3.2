#include "PskFactory.h"

#include "IMeshBuilderModule.h"
#include "PskUtils.h"
#include "PskReader.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Engine/SkinnedAssetCommon.h"

UObject* UPskFactory::Import(const FString& Filename, UObject* Parent, const FName Name, const EObjectFlags Flags, TMap<FString, FString> MaterialNameToPathMap, USkeleton* ExistingSkeleton, bool bCreateMaterialInstances)
{
	auto Data = FPskReader(Filename);
	if (!Data.bIsValid) return nullptr;
	const FString RootAssetName = Name.ToString();
	const FString ImportFolder = FPskUtils::ResolveImportFolder(Parent, RootAssetName);
	const FString RootAssetPackagePath = FPskUtils::ResolveRootAssetPackagePath(Parent, RootAssetName);

	TArray<FColor> VertexColorsByPoint;
	VertexColorsByPoint.Init(FColor::Black, Data.VertexColors.Num());
	if (Data.bHasVertexColors)
	{
		for (auto i = 0; i < Data.Wedges.Num(); i++)
		{
			auto FixedColor = Data.VertexColors[i];
			Swap(FixedColor.R, FixedColor.B);
			VertexColorsByPoint[Data.Wedges[i].PointIndex] = FixedColor;
		}
	}
	
	FSkeletalMeshImportData SkeletalMeshImportData;

	for (auto i = 0; i < Data.Normals.Num(); i++)
	{
		Data.Normals[i].Y = -Data.Normals[i].Y; // MIRROR_MESH
	}

	for (auto Vertex : Data.Vertices)
	{
		auto FixedVertex = Vertex;
		FixedVertex.Y = -FixedVertex.Y; // MIRROR_MESH
		SkeletalMeshImportData.Points.Add(FixedVertex);
		SkeletalMeshImportData.PointToRawMap.Add(SkeletalMeshImportData.Points.Num()-1);
	}
	
	auto WindingOrder = {2, 1, 0};
	for (const auto PskFace : Data.Faces)
	{
		SkeletalMeshImportData::FTriangle Face;
		Face.MatIndex = PskFace.MatIndex;
		Face.SmoothingGroups = 1;
		Face.AuxMatIndex = 0;

		for (auto VertexIndex : WindingOrder)
		{
			const auto WedgeIndex = PskFace.WedgeIndex[VertexIndex];
			const auto PskWedge = Data.Wedges[WedgeIndex];
			
			SkeletalMeshImportData::FVertex Wedge;
			Wedge.MatIndex = PskWedge.MatIndex;
			Wedge.VertexIndex = PskWedge.PointIndex;
			Wedge.Color = Data.bHasVertexColors ? VertexColorsByPoint[PskWedge.PointIndex] : FColor::Black;
			Wedge.UVs[0] = FVector2f(PskWedge.U, PskWedge.V);
			for (auto UVIdx = 0; UVIdx < Data.ExtraUVs.Num(); UVIdx++)
			{
				auto UV =  Data.ExtraUVs[UVIdx][PskFace.WedgeIndex[VertexIndex]];
				Wedge.UVs[UVIdx+1] = UV;
			}
			
			Face.WedgeIndex[VertexIndex] = SkeletalMeshImportData.Wedges.Add(Wedge);
			Face.TangentZ[VertexIndex] = Data.bHasVertexNormals ? Data.Normals[PskWedge.PointIndex] : FVector3f::ZeroVector;
			Face.TangentY[VertexIndex] = FVector3f::ZeroVector;
			Face.TangentX[VertexIndex] = FVector3f::ZeroVector;
		}
		Swap(Face.WedgeIndex[0], Face.WedgeIndex[2]);
		Swap(Face.TangentZ[0], Face.TangentZ[2]);

		SkeletalMeshImportData.Faces.Add(Face);
	}

	TArray<FString> AddedBoneNames;
	for (auto PskBone : Data.Bones)
	{
		SkeletalMeshImportData::FBone Bone;
		Bone.Name = PskBone.Name;
		if (AddedBoneNames.Contains(Bone.Name)) continue;
		
		Bone.NumChildren = PskBone.NumChildren;
		Bone.ParentIndex = PskBone.ParentIndex == -1 ? INDEX_NONE : PskBone.ParentIndex;
		
		auto PskBonePos = PskBone.BonePos;
		FTransform3f PskTransform;
		PskTransform.SetLocation(FVector3f(PskBonePos.Position.X, -PskBonePos.Position.Y, PskBonePos.Position.Z));
		PskTransform.SetRotation(FQuat4f(PskBonePos.Orientation.X, -PskBonePos.Orientation.Y, PskBonePos.Orientation.Z, PskBonePos.Orientation.W).GetNormalized());

		SkeletalMeshImportData::FJointPos BonePos;
		BonePos.Transform = PskTransform;
		BonePos.Length = PskBonePos.Length;
		BonePos.XSize = PskBonePos.XSize;
		BonePos.YSize = PskBonePos.YSize;
		BonePos.ZSize = PskBonePos.ZSize;

		Bone.BonePos = BonePos;
		SkeletalMeshImportData.RefBonesBinary.Add(Bone);
		AddedBoneNames.Add(Bone.Name);
	}

	for (auto PskInfluence : Data.Influences)
	{
		SkeletalMeshImportData::FRawBoneInfluence Influence;
		Influence.BoneIndex = PskInfluence.BoneIdx;
		Influence.VertexIndex = PskInfluence.PointIdx;
		Influence.Weight = PskInfluence.Weight;
		SkeletalMeshImportData.Influences.Add(Influence);
	}

	for (auto PskMaterial : Data.Materials)
	{
		SkeletalMeshImportData::FMaterial Material;
		Material.MaterialImportName = PskMaterial.MaterialName;

		if (bCreateMaterialInstances)
		{
			UObject* MatParent;
			auto FoundMaterialPath = MaterialNameToPathMap.Find(*Material.MaterialImportName);
			if (FoundMaterialPath != nullptr)
			{
				MatParent = CreatePackage(**FoundMaterialPath);
			}
			else
			{
				MatParent = CreatePackage(*FPaths::Combine(ImportFolder, PskMaterial.MaterialName));
			}
			
			auto MaterialAdd = FPskUtils::LocalFindOrCreateInPackage<UMaterialInstanceConstant>(UMaterialInstanceConstant::StaticClass(), MatParent->GetPathName(), PskMaterial.MaterialName, Flags);
			Material.Material = MaterialAdd;
		}
		SkeletalMeshImportData.Materials.Add(Material);
	}
	
	SkeletalMeshImportData.MaxMaterialIndex = SkeletalMeshImportData.Materials.Num()-1;

	SkeletalMeshImportData.bDiffPose = false;
	SkeletalMeshImportData.bHasNormals = Data.bHasVertexNormals;
	SkeletalMeshImportData.bHasTangents = false;
	SkeletalMeshImportData.bHasVertexColors = true;
	SkeletalMeshImportData.NumTexCoords = 1 + Data.ExtraUVs.Num(); 
	SkeletalMeshImportData.bUseT0AsRefPose = false;
	
	const FString SkeletonName = RootAssetName + TEXT("_Skeleton");
	USkeleton* Skeleton = ExistingSkeleton;
	const bool bCreatedSkeleton = Skeleton == nullptr;
	if (!Skeleton)
	{
		Skeleton = FPskUtils::LocalCreateInPackage<USkeleton>(USkeleton::StaticClass(), FPaths::Combine(ImportFolder, SkeletonName), SkeletonName, Flags);
	}
	if (!Skeleton)
	{
		return nullptr;
	}

	FReferenceSkeleton RefSkeleton;
	auto SkeletalDepth = 0;
	ProcessSkeleton(SkeletalMeshImportData, Skeleton, RefSkeleton, SkeletalDepth);

	TArray<FVector3f> LODPoints;
	TArray<SkeletalMeshImportData::FMeshWedge> LODWedges;
	TArray<SkeletalMeshImportData::FMeshFace> LODFaces;
	TArray<SkeletalMeshImportData::FVertInfluence> LODInfluences;
	TArray<int32> LODPointToRawMap;
	SkeletalMeshImportData.CopyLODImportData(LODPoints, LODWedges, LODFaces, LODInfluences, LODPointToRawMap);

	FSkeletalMeshLODModel LODModel;
	LODModel.NumTexCoords = FMath::Max<uint32>(1, SkeletalMeshImportData.NumTexCoords);
	
	const auto SkeletalMesh = FPskUtils::LocalCreateInPackage<USkeletalMesh>(USkeletalMesh::StaticClass(), RootAssetPackagePath, RootAssetName, Flags);
	SkeletalMesh->PreEditChange(nullptr);
	SkeletalMesh->InvalidateDeriveDataCacheGUID();
	SkeletalMesh->UnregisterAllMorphTarget();

	SkeletalMesh->GetRefBasesInvMatrix().Empty();
	SkeletalMesh->GetMaterials().Empty();
	SkeletalMesh->SetHasVertexColors(true);

	FSkeletalMeshModel* ImportedResource = SkeletalMesh->GetImportedModel();
	auto& SkeletalMeshLODInfos = SkeletalMesh->GetLODInfoArray();
	SkeletalMeshLODInfos.Empty();
	SkeletalMeshLODInfos.Add(FSkeletalMeshLODInfo());
	SkeletalMeshLODInfos[0].ReductionSettings.NumOfTrianglesPercentage = 1.0f;
	SkeletalMeshLODInfos[0].ReductionSettings.NumOfVertPercentage = 1.0f;
	SkeletalMeshLODInfos[0].ReductionSettings.MaxDeviationPercentage = 0.0f;
	SkeletalMeshLODInfos[0].LODHysteresis = 0.02f;

	ImportedResource->LODModels.Empty();
	ImportedResource->LODModels.Add(new FSkeletalMeshLODModel);
	SkeletalMesh->SetRefSkeleton(RefSkeleton);
	SkeletalMesh->CalculateInvRefMatrices();

	SkeletalMesh->SaveLODImportedData(0, SkeletalMeshImportData);
	FSkeletalMeshBuildSettings BuildOptions;
	BuildOptions.bRemoveDegenerates = true;
	BuildOptions.bRecomputeNormals = !Data.bHasVertexNormals;
	BuildOptions.bRecomputeTangents = true;
	BuildOptions.bUseMikkTSpace = true;
	SkeletalMesh->GetLODInfo(0)->BuildSettings = BuildOptions;
	SkeletalMesh->SetImportedBounds(FBoxSphereBounds(FBoxSphereBounds3f(FBox3f(SkeletalMeshImportData.Points))));

	auto& MeshBuilderModule = IMeshBuilderModule::GetForRunningPlatform();
	const FSkeletalMeshBuildParameters SkeletalMeshBuildParameters(SkeletalMesh, GetTargetPlatformManagerRef().GetRunningTargetPlatform(), 0, false);
	if (!MeshBuilderModule.BuildSkeletalMesh(SkeletalMeshBuildParameters))
	{
		SkeletalMesh->MarkAsGarbage();
		return nullptr;
	}

	for (auto Material : SkeletalMeshImportData.Materials)
	{
		SkeletalMesh->GetMaterials().Add(FSkeletalMaterial(Material.Material.Get()));
	}

	if (Data.bHasMorphData)
	{
		const FSkeletalMeshLODModel& BuiltLODModel = SkeletalMesh->GetImportedModel()->LODModels[0];
		const TArray<uint32>& RawPointIndices = BuiltLODModel.GetRawPointIndices();
		TMultiMap<int32, uint32> PointToVertexIndices;
		for (uint32 VertexIndex = 0; VertexIndex < static_cast<uint32>(RawPointIndices.Num()); ++VertexIndex)
		{
			PointToVertexIndices.Add(static_cast<int32>(RawPointIndices[VertexIndex]), VertexIndex);
		}

		bool bRegisteredMorphTargets = false;
		int32 DataPosition = 0;
		for (const auto& MorphInfo : Data.MorphInfos)
		{
			const FString MorphName = UTF8_TO_TCHAR(MorphInfo.Name);
			UMorphTarget* MorphTarget = NewObject<UMorphTarget>(SkeletalMesh, FName(*MorphName));
			TArray<FMorphTargetDelta> Deltas;
			Deltas.Reserve(MorphInfo.VertexCount);
			for (int32 i = DataPosition; i < DataPosition + MorphInfo.VertexCount && i < Data.MorphDatas.Num(); ++i)
			{
				const auto& MorphData = Data.MorphDatas[i];

				TArray<uint32> VertexIndices;
				PointToVertexIndices.MultiFind(MorphData.PointIdx, VertexIndices);
				if (VertexIndices.Num() == 0 && RawPointIndices.IsValidIndex(MorphData.PointIdx))
				{
					VertexIndices.Add(static_cast<uint32>(MorphData.PointIdx));
				}

				for (uint32 VertexIndex : VertexIndices)
				{
					FMorphTargetDelta Delta;
					Delta.PositionDelta = FVector3f(MorphData.PositionDelta.X, -MorphData.PositionDelta.Y, MorphData.PositionDelta.Z);
					Delta.TangentZDelta = FVector3f(MorphData.TangentZDelta.X, -MorphData.TangentZDelta.Y, MorphData.TangentZDelta.Z);
					Delta.SourceIdx = VertexIndex;
					Deltas.Add(Delta);
				}
			}

			MorphTarget->PopulateDeltas(Deltas, 0, BuiltLODModel.Sections, true, false, 0.0f);
			if (MorphTarget->HasValidData())
			{
				bRegisteredMorphTargets |= SkeletalMesh->RegisterMorphTarget(MorphTarget, false);
			}
			DataPosition += MorphInfo.VertexCount;
		}

		if (bRegisteredMorphTargets)
		{
			SkeletalMesh->InitMorphTargetsAndRebuildRenderData();
		}
	}
	
	SkeletalMesh->PostEditChange();
	
	SkeletalMesh->SetSkeleton(Skeleton);
	Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
	
	FAssetRegistryModule::AssetCreated(SkeletalMesh);
	SkeletalMesh->MarkPackageDirty();

	Skeleton->PostEditChange();
	if (bCreatedSkeleton)
	{
		FAssetRegistryModule::AssetCreated(Skeleton);
	}
	Skeleton->MarkPackageDirty();

	return SkeletalMesh;
}

void UPskFactory::ProcessSkeleton(const FSkeletalMeshImportData& ImportData, const USkeleton* Skeleton, FReferenceSkeleton& OutRefSkeleton, int& OutSkeletalDepth)
{
	const auto RefBonesBinary = ImportData.RefBonesBinary;
	OutRefSkeleton.Empty();
	
	FReferenceSkeletonModifier RefSkeletonModifier(OutRefSkeleton, Skeleton);
	
	for (const auto Bone : RefBonesBinary)
	{
		const FMeshBoneInfo BoneInfo(FName(*Bone.Name), Bone.Name, Bone.ParentIndex);
		RefSkeletonModifier.Add(BoneInfo, FTransform(Bone.BonePos.Transform));
	}

    OutSkeletalDepth = 0;

    TArray<int> SkeletalDepths;
    SkeletalDepths.Empty(ImportData.RefBonesBinary.Num());
    SkeletalDepths.AddZeroed(ImportData.RefBonesBinary.Num());
    for (auto b = 0; b < OutRefSkeleton.GetNum(); b++)
    {
        const auto Parent = OutRefSkeleton.GetParentIndex(b);
        auto Depth  = 1.0f;

        SkeletalDepths[b] = 1.0f;
        if (Parent != INDEX_NONE)
        {
            Depth += SkeletalDepths[Parent];
        }
        if (OutSkeletalDepth < Depth)
        {
            OutSkeletalDepth = Depth;
        }
        SkeletalDepths[b] = Depth;
    }
}
