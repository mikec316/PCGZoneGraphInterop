#include "ZoneGraphToPoints.h"

#include "PCGComponent.h"
#include "PCGSubsystem.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSpatialData.h"
#include "Grid/PCGPartitionActor.h"
#include "Helpers/PCGHelpers.h"

#include "Algo/AnyOf.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

#include "ZoneShapeComponent.h"
#include "Data/PCGSplineData.h"
#include "ZoneGraphSettings.h" // Include the settings header
#include "ZoneGraphTypes.h"

#define LOCTEXT_NAMESPACE "PCGZoneGraphToPointsElement"

#if WITH_EDITOR
FText UPCGZoneGraphToPointsSettings::GetNodeTooltipText() const
{
    return LOCTEXT("ZoneGraphToPointsTooltip", "Builds a collection of PCG-compatible data from the selected actors.");
}

void UPCGZoneGraphToPointsSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UPCGZoneGraphToPointsSettings, ActorSelector))
    {
        if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(FPCGActorSelectorSettings, ActorSelection))
        {
            // Make sure that when switching away from the 'by class' selection, we actually break that data dependency
            if (ActorSelector.ActorSelection != EPCGActorSelection::ByClass)
            {
                ActorSelector.ActorSelectionClass = GetDefaultActorSelectorClass();
            }
        }
    }
}

#endif

TSubclassOf<AActor> UPCGZoneGraphToPointsSettings::GetDefaultActorSelectorClass() const
{
    return TSubclassOf<AActor>();
}

void UPCGZoneGraphToPointsSettings::PostLoad()
{
    Super::PostLoad();

    if (ActorSelector.ActorSelection != EPCGActorSelection::ByClass)
    {
        ActorSelector.ActorSelectionClass = GetDefaultActorSelectorClass();
    }
}

FName UPCGZoneGraphToPointsSettings::AdditionalTaskName() const
{
#if WITH_EDITOR
    return FName(ActorSelector.GetTaskName().ToString());
#else
    return Super::AdditionalTaskName();
#endif
}

FPCGElementPtr UPCGZoneGraphToPointsSettings::CreateElement() const
{
    return MakeShared<FPCGZoneGraphToPointsElement>();
}

EPCGDataType UPCGZoneGraphToPointsSettings::GetCurrentPinTypes(const UPCGPin* InPin) const
{
    check(InPin);

    if (InPin->IsOutputPin())
    {
        if (Mode == EPCGGetZoneGraphToPointsMode::GetSinglePoint)
        {
            return EPCGDataType::Point;
        }
        else if (Mode == EPCGGetZoneGraphToPointsMode::GetDataFromProperty)
        {
            return EPCGDataType::Param;
        }
        else if (Mode == EPCGGetZoneGraphToPointsMode::ParseActorComponents)
        {
            return EPCGDataType::Spline;
        }
    }

    return Super::GetCurrentPinTypes(InPin);
}

TArray<FPCGPinProperties> UPCGZoneGraphToPointsSettings::OutputPinProperties() const
{
    TArray<FPCGPinProperties> Pins = Super::OutputPinProperties();

    if (Mode == EPCGGetZoneGraphToPointsMode::GetDataFromPCGComponent || Mode == EPCGGetZoneGraphToPointsMode::GetDataFromPCGComponentOrParseComponents)
    {
        for (const FName& Pin : ExpectedPins)
        {
            Pins.Emplace(Pin);
        }
    }

    return Pins;
}

FPCGContext* FPCGZoneGraphToPointsElement::Initialize(const FPCGDataCollection& InputData, TWeakObjectPtr<UPCGComponent> SourceComponent, const UPCGNode* Node)
{
    FPCGZoneGraphToPointsContext* Context = new FPCGZoneGraphToPointsContext();
    Context->InputData = InputData;
    Context->Node = Node;

    return Context;
}

FPCGContext* FPCGZoneGraphToPointsElement::Initialize(const FPCGInitializeElementParams& InParams) {
    FPCGZoneGraphToPointsContext* Context = new FPCGZoneGraphToPointsContext();
    if (InParams.InputData) {
        Context->InputData = *InParams.InputData;
    }
    Context->Node = InParams.Node;

    return Context;
    
}

bool FPCGZoneGraphToPointsElement::ExecuteInternal(FPCGContext* InContext) const
{
    TRACE_CPUPROFILER_EVENT_SCOPE(FPCGZoneGraphToPointsElement::Execute);

    check(InContext);
    FPCGZoneGraphToPointsContext* Context = static_cast<FPCGZoneGraphToPointsContext*>(InContext);

    const UPCGZoneGraphToPointsSettings* Settings = Context->GetInputSettings<UPCGZoneGraphToPointsSettings>();
    check(Settings);

    if (!Context->bPerformedQuery)
    {
        TFunction<bool(const AActor*)> BoundsCheck = [](const AActor*) -> bool { return true; };
        UPCGComponent* PCGComponent = Cast<UPCGComponent>(Context->ExecutionSource.Get());
        const AActor* Self = PCGComponent ? PCGComponent->GetOwner() : nullptr;

        if (Self && Settings->ActorSelector.bMustOverlapSelf)
        {
            // Capture ActorBounds by value because it goes out of scope
            const FBox ActorBounds = PCGHelpers::GetActorBounds(Self);
            BoundsCheck = [Settings, ActorBounds, PCGComponent](const AActor* OtherActor) -> bool
            {
                const FBox OtherActorBounds = OtherActor ? PCGHelpers::GetGridBounds(OtherActor, PCGComponent) : FBox(EForceInit::ForceInit);
                return ActorBounds.Intersect(OtherActorBounds);
            };
        }

        TFunction<bool(const AActor*)> SelfIgnoreCheck = [](const AActor*) -> bool { return true; };
        if (Self && Settings->ActorSelector.bIgnoreSelfAndChildren)
        {
            SelfIgnoreCheck = [Self](const AActor* OtherActor) -> bool
            {
                // Check if OtherActor is a child of self
                const AActor* CurrentOtherActor = OtherActor;
                while (CurrentOtherActor)
                {
                    if (CurrentOtherActor == Self)
                    {
                        return false;
                    }

                    CurrentOtherActor = CurrentOtherActor->GetParentActor();
                }

                // Check if Self is a child of OtherActor
                const AActor* CurrentSelfActor = Self;
                while (CurrentSelfActor)
                {
                    if (CurrentSelfActor == OtherActor)
                    {
                        return false;
                    }

                    CurrentSelfActor = CurrentSelfActor->GetParentActor();
                }

                return true;
            };
        }

        Context->FoundActors = PCGActorSelector::FindActors(Settings->ActorSelector, PCGComponent, BoundsCheck, SelfIgnoreCheck);
        Context->bPerformedQuery = true;

        if (Context->FoundActors.IsEmpty())
        {
            PCGE_LOG(Warning, GraphAndLog, LOCTEXT("NoActorFound", "No matching actor was found"));
            return true;
        }

        // If we're looking for PCG component data, we might have to wait for it.
        if (Settings->Mode == EPCGGetZoneGraphToPointsMode::GetDataFromPCGComponent || Settings->Mode == EPCGGetZoneGraphToPointsMode::GetDataFromPCGComponentOrParseComponents)
        {
            TArray<FPCGTaskId> WaitOnTaskIds;
            for (AActor* Actor : Context->FoundActors)
            {
                GatherWaitTasks(Actor, Context, WaitOnTaskIds);
            }

            if (!WaitOnTaskIds.IsEmpty())
            {
                
                UWorld* World = Context->ExecutionSource->GetExecutionState().GetWorld();
                if (UPCGSubsystem* Subsystem = UPCGSubsystem::GetInstance(World))
                {
                    // Add a trivial task after these generations that wakes up this task
                    Context->bIsPaused = true;

                    

                    Subsystem->ScheduleGeneric(
                        [Context]()
                        {
                            // Wake up the current task
                            Context->bIsPaused = false;
                            return true;
                        }, PCGComponent, WaitOnTaskIds);

                    return false;
                }
                else
                {
                    PCGE_LOG(Error, GraphAndLog, LOCTEXT("UnableToWaitForGenerationTasks", "Unable to wait for end of generation tasks"));
                }
            }
        }
    }

    if (Context->bPerformedQuery)
    {
        ProcessActors(Context, Settings, Context->FoundActors);
    }

    return true;
}

void FPCGZoneGraphToPointsElement::GatherWaitTasks(AActor* FoundActor, FPCGContext* Context, TArray<FPCGTaskId>& OutWaitTasks) const
{
    if (!FoundActor)
    {
        return;
    }

    // We will prevent gathering the current execution - this task cannot wait on itself
    //
    UPCGComponent* PCGComponent = Cast<UPCGComponent>(Context->ExecutionSource.Get());
    AActor* ThisOwner = PCGComponent ? PCGComponent->GetOwner() : nullptr;

    TInlineComponentArray<UPCGComponent*, 1> PCGComponents;
    FoundActor->GetComponents(PCGComponents);

    for (UPCGComponent* Component : PCGComponents)
    {
        if (Component->IsGenerating() && Component->GetOwner() != ThisOwner)
        {
            OutWaitTasks.Add(Component->GetGenerationTaskId());
        }
    }
}

void FPCGZoneGraphToPointsElement::ProcessActors(FPCGContext* Context, const UPCGZoneGraphToPointsSettings* Settings, const TArray<AActor*>& FoundActors) const
{
    // Special case:
    // If we're asking for single point with the merge single point data, we can do a more efficient process
    if (Settings->Mode == EPCGGetZoneGraphToPointsMode::GetSinglePoint && Settings->bMergeSinglePointData && FoundActors.Num() > 1)
    {
        MergeActorsIntoPointData(Context, Settings, FoundActors);
    }
    else
    {
        for (AActor* Actor : FoundActors)
        {
            ProcessActor(Context, Settings, Actor);
        }
    }
}

void FPCGZoneGraphToPointsElement::MergeActorsIntoPointData(FPCGContext* Context, const UPCGZoneGraphToPointsSettings* Settings, const TArray<AActor*>& FoundActors) const
{
    check(Context);

    // At this point in time, the partition actors behave slightly differently, so if we are in the case where
    // we have one or more partition actors, we'll go through the normal process and do post-processing to merge the point data instead.
    const bool bContainsPartitionActors = Algo::AnyOf(FoundActors, [](const AActor* Actor) { return Cast<APCGPartitionActor>(Actor) != nullptr; });

    UPCGComponent* PCGComponent = Cast<UPCGComponent>(Context->ExecutionSource.Get());
    
    if (!bContainsPartitionActors)
    {
        UPCGPointData* Data = NewObject<UPCGPointData>();
        bool bHasData = false;

        for (AActor* Actor : FoundActors)
        {
            if (Actor)
            {
                Data->AddSinglePointFromActor(Actor);
                bHasData = true;
            }
         }

        if (bHasData)
        {
            FPCGTaggedData& TaggedData = Context->OutputData.TaggedData.Emplace_GetRef();
            TaggedData.Data = Data;
        }
    }
    else // Stripped-down version of the normal code path with bParseActor = false
    {
        FPCGDataCollection DataToMerge;
        const bool bParseActor = false;

        for (AActor* Actor : FoundActors)
        {
            if (Actor)
            {
                FPCGDataCollection Collection = UPCGComponent::CreateActorPCGDataCollection(Actor, PCGComponent, EPCGDataType::Any, bParseActor);
                DataToMerge.TaggedData += Collection.TaggedData;
            }
        }

        // COMMENTED THIS BECAUSE IT WOULDN'T COMPILE
        // ANYWAY WE OUTPUT THE DATA BY HAND
        
        /* 
        // Perform point data-to-point data merge
        if (DataToMerge.TaggedData.Num() > 1)
        {
            UPCGMergeSettings* MergeSettings = NewObject<UPCGMergeSettings>();
            FPCGMergeElement MergeElement;
            FPCGContext* MergeContext = MergeElement.Initialize(DataToMerge, Context->SourceComponent, nullptr);
            MergeContext->AsyncState.NumAvailableTasks = Context->AsyncState.NumAvailableTasks;
            MergeContext->InputData.TaggedData.Emplace_GetRef().Data = MergeSettings;

            while (!MergeElement.Execute(MergeContext))
            {
            }

            Context->OutputData = MergeContext->OutputData;
            delete MergeContext;
        }
        else if (DataToMerge.TaggedData.Num() == 1)
        {
            Context->OutputData.TaggedData = DataToMerge.TaggedData;
        }
        */
    }
}

void FPCGZoneGraphToPointsElement::ProcessActor(FPCGContext* Context, const UPCGZoneGraphToPointsSettings* Settings, AActor* FoundActor) const
{
    check(Context);
    check(Settings);

    if (!FoundActor || !IsValid(FoundActor))
    {
        return;
    }

    UPCGComponent* PCGComponent = Cast<UPCGComponent>(Context->ExecutionSource.Get());
    const AActor* ThisOwner = PCGComponent ? PCGComponent->GetOwner() : nullptr;
    
    if (FoundActor == ThisOwner) { return; } // Skip self
    
    const TSet<UActorComponent*>& AllComponents = FoundActor->GetComponents(); // Get all components on the actor

    const FTransform WorldActorTransform = FoundActor->GetTransform();
    
    for (const UActorComponent* MaybeZoneShape : AllComponents)
    {
        const UZoneShapeComponent* ZoneShape = Cast<UZoneShapeComponent>(MaybeZoneShape);
        if (!ZoneShape) { continue; } // Not a ZoneShape :(
        
        TConstArrayView<FZoneShapePoint> ZoneShapePoints = ZoneShape->GetPoints();
        if (ZoneShapePoints.IsEmpty()) { continue; } // no points

        UPCGPointData* NewPointData = NewObject<UPCGPointData>();
        TArray<FPCGPoint>& MutablePoints = NewPointData->GetMutablePoints();

        FPCGMetadataAttribute<FVector>* OutTangent = NewPointData->Metadata->CreateAttribute(FName("TangentOut"), FVector::Zero(), true, false);
        FPCGMetadataAttribute<FVector>* InTangent = NewPointData->Metadata->CreateAttribute(FName("TangentIn"), FVector::Zero(), true, false);

        UPCGSplineData* SplineData = NewObject<UPCGSplineData>();
        TArray<FSplinePoint> SplinePoints;
        SplinePoints.Reserve(MutablePoints.Num());
        const FTransform SplineActorTransform = FoundActor->GetTransform();

        
        // Retrieve the full list of tags from UZoneGraphSettings
        TConstArrayView<FZoneGraphTagInfo> AllTags = GetDefault<UZoneGraphSettings>()->GetTagInfos();
        
        FZoneGraphTagMask ZoneShapeTags = ZoneShape->GetTags();
        
        TSet<FString> TagInfo;
        
        
        // Loop through the tags and check which ones are active
        for (const FZoneGraphTagInfo& TagInfos : AllTags)
        {
            if (ZoneShapeTags.Contains(TagInfos.Tag))
            {
                
            TagInfo.Add(TagInfos.Name.ToString());
            }

        }
         //Retrieve LaneProfiles, add to tags, extract widths and create attribute

        FZoneLaneProfile ZoneGraphLanes;
        ZoneShape->GetSplineLaneProfile(ZoneGraphLanes);
        double ZoneLaneWidth = ZoneGraphLanes.GetLanesTotalWidth();
       
        TagInfo.Add(ZoneGraphLanes.Name.ToString());
       

        for(const FZoneShapePoint Pt : ZoneShapePoints)
        {
            FPCGPoint& NewPoint = MutablePoints.Emplace_GetRef();
            NewPointData->Metadata->InitializeOnSet(NewPoint.MetadataEntry);
            
            NewPoint.Transform.SetLocation(WorldActorTransform.TransformPosition(Pt.Position)); // Transform position so it's not in local space
            OutTangent->SetValue(NewPoint.MetadataEntry, Pt.GetOutControlPoint() *-1);
            InTangent->SetValue(NewPoint.MetadataEntry, Pt.GetInControlPoint()*-1);

        }
        
        ESplinePointType::Type PointType = ESplinePointType::Linear;

        for(int32 PointIndex = 0; PointIndex < MutablePoints.Num(); ++PointIndex)
        {
            const FPCGPoint& Point = MutablePoints[PointIndex];
            const FTransform& PointTransform = Point.Transform;
            const FVector LocalPosition = PointTransform.GetLocation() - SplineActorTransform.GetLocation();

            SplinePoints.Emplace(static_cast<float>(PointIndex),
                LocalPosition,
                InTangent ? InTangent->GetValueFromItemKey(Point.MetadataEntry) : FVector::ZeroVector,
                OutTangent ? OutTangent->GetValueFromItemKey(Point.MetadataEntry) : FVector::ZeroVector,
                PointTransform.GetRotation().Rotator(),
                PointTransform.GetScale3D(),
                PointType);
        }
        //FPCGSplineStruct SplineStruct;
        //SplineStruct.Initialize(SplinePoints, false, FTransform(SplineActorTransform.GetLocation()));
        SplineData->Initialize(SplinePoints, false, FTransform(SplineActorTransform.GetLocation()));
        //SplineData->Initialize(SplineStruct);
        SplineData->Metadata->CreateAttribute(FName("LaneWidth"), ZoneLaneWidth, true, false);
        
        Context->OutputData.TaggedData.Emplace(SplineData);
        for (FPCGTaggedData& OutputTaggedData : Context->OutputData.TaggedData)
        {
            OutputTaggedData.Tags.Append(TagInfo);
        }
        
    }
}

#undef LOCTEXT_NAMESPACE
