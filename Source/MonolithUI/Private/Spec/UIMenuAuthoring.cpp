#include "Spec/UIMenuAuthoring.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Blueprint/WidgetNavigation.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MonolithUICommon.h"
#if WITH_COMMONUI
#include "CommonActivatableWidget.h"
#include "Widgets/CommonActivatableWidgetContainer.h"
#endif

namespace MonolithUI::MenuAuthoring
{
    template<typename T> static T* Node(UEdGraph* Graph)
    {
        T* Result = NewObject<T>(Graph);
        Graph->AddNode(Result, false, false);
        Result->CreateNewGuid();
        return Result;
    }

    static bool Connect(UEdGraphPin* A, UEdGraphPin* B)
    {
        return A && B && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B);
    }

    static void PublishWidgetVariables(UWidgetBlueprint* WBP)
    {
        // MarkBlueprintAsStructurallyModified can deliberately skip a freshly
        // created Blueprint (BS_BeingCreated). Generate the skeleton explicitly
        // after registering widget GUIDs, before allocating variable-get pins.
        MonolithUI::ReconcileWidgetVariableGuids(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::RegenerateSkeletonOnly);
    }

    bool ApplyScreen(UWidgetBlueprint* WBP, const FString& FocusTarget,
        const TArray<TSharedPtr<FJsonObject>>& Navigation, FString& Error)
    {
        if (!WBP || !WBP->WidgetTree) { Error = TEXT("Missing widget tree"); return false; }
        static const TMap<FString, EUINavigation> Directions = {
            {TEXT("up"), EUINavigation::Up}, {TEXT("down"), EUINavigation::Down},
            {TEXT("left"), EUINavigation::Left}, {TEXT("right"), EUINavigation::Right},
            {TEXT("next"), EUINavigation::Next}, {TEXT("previous"), EUINavigation::Previous}};
        for (const auto& Entry : Navigation)
        {
            UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*Entry->GetStringField(TEXT("widget"))));
            UWidget* Target = WBP->WidgetTree->FindWidget(FName(*Entry->GetStringField(TEXT("target"))));
            const auto* Direction = Directions.Find(Entry->GetStringField(TEXT("direction")).ToLower());
            if (!Widget || !Target || !Direction) { Error = TEXT("Invalid navigation reference"); return false; }
            Widget->Modify();
            Widget->SetNavigationRuleExplicit(*Direction, Target);
            // Editor templates have no Slate instances. Persist the name used by ResolveRules.
            Widget->Navigation->GetNavigationData(*Direction).WidgetToFocus = Target->GetFName();
        }
        if (FocusTarget.IsEmpty()) return true;
#if WITH_COMMONUI
        if (!WBP->ParentClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
        { Error = TEXT("focus_table requires a CommonActivatableWidget screen"); return false; }
        const FName FunctionName(TEXT("BP_GetDesiredFocusTarget"));
        const TArray<TObjectPtr<UEdGraph>> ExistingFunctions = WBP->FunctionGraphs;
        for (UEdGraph* Existing : ExistingFunctions)
        {
            if (Existing && Existing->GetFName() == FunctionName)
            {
                const bool bOwned = Existing->Nodes.ContainsByPredicate([](const UEdGraphNode* N)
                { return N && N->NodeComment == TEXT("Monolith.Menu.DesiredFocus"); });
                if (!bOwned) { Error = TEXT("Existing desired-focus override would be overwritten"); return false; }
                FBlueprintEditorUtils::RemoveGraph(WBP, Existing, EGraphRemoveFlags::MarkTransient);
            }
        }
        UFunction* Signature = WBP->ParentClass->FindFunctionByName(FunctionName);
        if (!Signature) { Error = TEXT("CommonUI focus function is unavailable"); return false; }
        UWidget* FocusWidget = WBP->WidgetTree->FindWidget(FName(*FocusTarget));
        if (!FocusWidget) { Error = TEXT("Missing focus widget"); return false; }
        FocusWidget->bIsVariable = true;
        PublishWidgetVariables(WBP);
        UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(WBP, FunctionName,
            UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        // This is an inherited override. The UFunction overload creates a NEW
        // user-signature function, and can duplicate inherited return pins.
        UClass* SignatureClass = WBP->ParentClass;
        FBlueprintEditorUtils::AddFunctionGraph(WBP, Graph, false, SignatureClass);
        UK2Node_FunctionResult* Return = nullptr;
        for (UEdGraphNode* N : Graph->Nodes) if (auto* R = Cast<UK2Node_FunctionResult>(N)) Return = R;
        auto* GetWidget = Node<UK2Node_VariableGet>(Graph);
        GetWidget->NodeComment = TEXT("Monolith.Menu.DesiredFocus");
        GetWidget->VariableReference.SetSelfMember(FocusWidget->GetFName());
        GetWidget->AllocateDefaultPins();
        UEdGraphPin* ValuePin = GetWidget->GetValuePin();
        if (ValuePin)
        {
            ValuePin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            ValuePin->PinType.PinSubCategoryObject = FocusWidget->GetClass();
        }
        UEdGraphPin* ReturnPin = Return ? Return->FindPin(Signature->GetReturnProperty()->GetFName()) : nullptr;
        if (!Connect(ValuePin, ReturnPin))
        {
            Error = FString::Printf(TEXT("Failed to wire desired-focus override (getter=%s, return=%s, skeleton_widget=%s)"),
                ValuePin ? *ValuePin->PinName.ToString() : TEXT("missing"),
                ReturnPin ? *ReturnPin->PinName.ToString() : TEXT("missing"),
                FindFProperty<FProperty>(WBP->SkeletonGeneratedClass, FocusWidget->GetFName()) ? TEXT("present") : TEXT("missing"));
            return false;
        }
        return true;
#else
        Error = TEXT("CommonUI is unavailable"); return false;
#endif
    }

    bool ApplyLayers(UWidgetBlueprint* WBP, const TArray<TSharedPtr<FJsonObject>>& Layers,
        const TMap<FString, FString>& ScreenPaths, FString& Error)
    {
#if WITH_COMMONUI
        UOverlay* Root = WBP && WBP->WidgetTree ? Cast<UOverlay>(WBP->WidgetTree->RootWidget) : nullptr;
        if (!Root) { Error = TEXT("Menu host requires an Overlay root"); return false; }
        const TArray<TObjectPtr<UEdGraph>> ExistingGraphs = WBP->UbergraphPages;
        for (UEdGraph* Existing : ExistingGraphs)
        {
            if (Existing && Existing->Nodes.ContainsByPredicate([](const UEdGraphNode* N)
                { return N && N->NodeComment == TEXT("Monolith.Menu.InitializeLayers"); }))
                FBlueprintEditorUtils::RemoveGraph(WBP, Existing, EGraphRemoveFlags::MarkTransient);
        }
        UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(WBP, TEXT("MonolithMenuInitialization"),
            UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(WBP, Graph);
        auto* Event = Node<UK2Node_Event>(Graph);
        Event->NodeComment = TEXT("Monolith.Menu.InitializeLayers");
        // Containers need their Slate switcher, which does not exist during OnInitialized.
        Event->EventReference.SetExternalMember(TEXT("Construct"), UUserWidget::StaticClass());
        Event->bOverrideFunction = true;
        Event->AllocateDefaultPins();
        UEdGraphPin* Exec = Event->FindPin(UEdGraphSchema_K2::PN_Then);
        for (const auto& Layer : Layers)
        {
            UCommonActivatableWidgetStack* Stack = WBP->WidgetTree->ConstructWidget<UCommonActivatableWidgetStack>(
                UCommonActivatableWidgetStack::StaticClass(), FName(*Layer->GetStringField(TEXT("id"))));
            Stack->bIsVariable = true;
            UOverlaySlot* Slot = Root->AddChildToOverlay(Stack);
            Slot->SetHorizontalAlignment(HAlign_Fill);
            Slot->SetVerticalAlignment(VAlign_Fill);
            PublishWidgetVariables(WBP);
            auto* GetStack = Node<UK2Node_VariableGet>(Graph);
            GetStack->VariableReference.SetSelfMember(Stack->GetFName());
            GetStack->AllocateDefaultPins();
            UEdGraphPin* StackPin = GetStack->GetValuePin();
            if (!StackPin) { Error = TEXT("Failed to create stack variable pin"); return false; }
            StackPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            StackPin->PinType.PinSubCategoryObject = Stack->GetClass();
            auto* Clear = Node<UK2Node_CallFunction>(Graph);
            Clear->SetFromFunction(UCommonActivatableWidgetContainerBase::StaticClass()->FindFunctionByName(TEXT("ClearWidgets")));
            Clear->AllocateDefaultPins();
            if (!Connect(Exec, Clear->GetExecPin()) || !Connect(StackPin, Clear->FindPin(UEdGraphSchema_K2::PN_Self)))
            { Error = TEXT("Failed to wire stack reset"); return false; }
            Exec = Clear->GetThenPin();
            for (const auto& Screen : Layer->GetArrayField(TEXT("screens")))
            {
                const FString* Path = ScreenPaths.Find(Screen->AsString());
                UWidgetBlueprint* ScreenWBP = Path ? LoadObject<UWidgetBlueprint>(nullptr, **Path) : nullptr;
                if (!ScreenWBP || !ScreenWBP->GeneratedClass || !ScreenWBP->GeneratedClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
                { Error = TEXT("Layer screen must compile as CommonActivatableWidget"); return false; }
                auto* Push = Node<UK2Node_CallFunction>(Graph);
                Push->SetFromFunction(UCommonActivatableWidgetContainerBase::StaticClass()->FindFunctionByName(TEXT("BP_AddWidget")));
                Push->AllocateDefaultPins();
                auto* ClassPin = Push->FindPin(TEXT("ActivatableWidgetClass"));
                if (!ClassPin || !Connect(Exec, Push->GetExecPin()) || !Connect(StackPin, Push->FindPin(UEdGraphSchema_K2::PN_Self)))
                { Error = TEXT("Failed to wire CommonUI stack initialization"); return false; }
                ClassPin->DefaultObject = ScreenWBP->GeneratedClass;
                Exec = Push->GetThenPin();
            }
        }
        return true;
#else
        Error = TEXT("CommonUI is unavailable"); return false;
#endif
    }
}
