#!/usr/bin/env python3
"""D-261 (B-039): generate UUniTuningSettings (Project Settings > Plugins > Unified Universe Tuning)
from the plugin's uu.* console variables, so the editor page and the console are one list.

    python3 Tools/gen_tuning_settings.py          # write the header and the .cpp
    python3 Tools/gen_tuning_settings.py --check  # exit 1 if they differ from what would be written

Each console variable becomes one property bound to it (meta UniCVar): an edit sets the variable
(as typing it would), the variable's value at start-up is the property's default, and a saved
value (DefaultGame.ini) is applied when the subsystem starts. Variables whose help says they apply
at uu.Enable (or that reroll stars) carry UniReenable: an edit re-enables the scene where the
camera is, after a short pause. T4.static.tuning_settings checks the files are current."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
SOURCES = [
    "Source/UnifiedUniverse/Private/UniSubsystem.cpp",
    "Source/UnifiedUniverseRender/Private/UniShaders.cpp",
    "Source/UnifiedUniverseRender/Private/UniRenderer.cpp",
]
HEADER = "Source/UnifiedUniverse/Public/UniTuningSettings.h"
CPP = "Source/UnifiedUniverse/Private/UniTuningSettings.cpp"

PAT = re.compile(r'TAutoConsoleVariable<(\w+)>\s+(\w+)\(\s*TEXT\("(uu\.[^"]+)"\)\s*,\s*(.+?),\s*((?:TEXT\("(?:[^"\\]|\\.)*"\)\s*)+)(?:,\s*(ECVF_\w+(?:\s*\|\s*ECVF_\w+)*))?\s*\)\s*;', re.S)

# Start-up only: not worth a page entry (they act once, when play begins).
SKIP = {"uu.AutoStart", "uu.AutoFly"}

# Categories, first match wins: (prefix or exact name, category).
CATEGORIES = [
    ("uu.Volume.StepFoot", "March|Steps"),
    ("uu.Volume.OpaqueCutoff", "March|Steps"),
    ("uu.Volume.SegmentTaps", "March|Paradigms"),
    ("uu.VolumeStepRatio", "March|Steps"),
    ("uu.VolumeMaxSteps", "March|Steps"),
    ("uu.VolumeStartPc", "March|Steps"),
    ("uu.VolumeHintScale", "March|Steps"),
    ("uu.VolumeMaxSubsamples", "March|Steps"),
    ("uu.VolumeWebStepRatio", "March|Steps"),
    ("uu.VolumeWebTravelHint", "March|Steps"),
    ("uu.VolumeTravelPixelScale", "March|Steps"),
    ("uu.Nebula.StepRatio", "March|Steps"),
    ("uu.GalaxyMap", "March|Paradigms", True),
    ("uu.Nebula.Grid", "March|Paradigms", True),
    ("uu.Nebula.Refine", "March|Paradigms"),
    ("uu.GalaxyMap.", "March|Galaxy Map"),
    ("uu.Nebula.Grid", "March|Voxel Grid (nebulae)"),
    ("uu.VolumeJitter", "March|Sampling and Noise"),
    ("uu.VolumeBlueNoise", "March|Sampling and Noise"),
    ("uu.VolumePrefilter", "March|Sampling and Noise"),
    ("uu.VolumeLutStart", "March|Sampling and Noise"),
    ("uu.VolumeDenoise", "March|Sampling and Noise"),
    ("uu.VolumeTemporal", "March|Temporal"),
    ("uu.VolumeAccumulate", "March|Temporal"),
    ("uu.VolumeAutoHold", "March|Temporal"),
    ("uu.VolumeResolve", "March|Resolution"),
    ("uu.VolumeDivisor", "March|Resolution"),
    ("uu.VolumeInterleave", "March|Resolution"),
    ("uu.VolumeMaxMarchPixels", "March|Resolution"),
    ("uu.VolumeTargetFps", "March|Resolution"),
    ("uu.VolumeBeforeTSR", "March|Resolution"),
    ("uu.VolumeFar", "March|Far Cache"),
    ("uu.VolumeDebug", "Debug"),
    ("uu.Volume.DustGain", "Galaxy|Balance"),
    ("uu.Volume", "March|Other"),
    ("uu.GalaxyTexture", "Textures"),
    ("uu.GalaxyTextures", "Textures"),
    ("uu.WebTex", "Textures"),
    ("uu.WebSource", "Textures"),
    ("uu.Galaxy.Knot", "Galaxy|Shape and Noise"),
    ("uu.Galaxy.DustStructure", "Galaxy|Shape and Noise"),
    ("uu.Galaxy.LightStructure", "Galaxy|Shape and Noise"),
    ("uu.Galaxy.StructureFreq", "Galaxy|Shape and Noise"),
    ("uu.Galaxy.", "Galaxy|Balance"),
    ("uu.GalaxyPalettes", "Galaxy|Balance"),
    ("uu.Nebula", "Nebulae"),
    ("uu.Star.", "Population"),
    ("uu.Cluster.", "Population|Clusters"),
    ("uu.Group.", "Population|Clusters"),
    ("uu.Sprite", "Sprites"),
    ("uu.MotionWeb", "Web"),
    ("uu.SkyWeb", "Web"),
    ("uu.WebDebug", "Debug"),
    ("uu.Warp", "Travel"),
    ("uu.Travel", "Travel"),
    ("uu.Shift", "Travel"),
    ("uu.SkyLight", "Lighting"),
    ("uu.LightRig", "Lighting"),
    ("uu.Background", "Background"),
    ("uu.Perf", "Debug"),
    ("uu.Shaders", "Debug"),
]
# The page's order (the details panel keeps declaration order).
CAT_ORDER = ["March|Paradigms", "March|Steps", "March|Sampling and Noise", "March|Temporal", "March|Resolution", "March|Galaxy Map",
             "March|Voxel Grid (nebulae)", "March|Far Cache", "March|Other", "Textures", "Galaxy|Shape and Noise", "Galaxy|Balance",
             "Nebulae", "Population", "Population|Clusters", "Sprites", "Web", "Travel", "Lighting", "Background",
             "Streaming and Generation", "Debug"]
REENABLE_PREFIX = ("uu.Cluster.", "uu.Group.", "uu.Star.", "uu.WebTex", "uu.WebSource", "uu.WebTexture", "uu.GalaxyTextures", "uu.GalaxyPalettes", "uu.GalaxyTexture.")


def category(name):
    """(order, category): the first entry that matches (an exact entry only its own name)."""
    for k, e in enumerate(CATEGORIES):
        p, c = e[0], e[1]
        exact = len(e) > 2 and e[2]
        if name == p or (not exact and name.startswith(p)):
            return k, c
    return len(CATEGORIES), "Streaming and Generation"


def parse():
    out = {}
    for f in SOURCES:
        src = open(os.path.join(PLUGIN, f)).read()
        for t, var, name, default, texts, flags in PAT.findall(src):
            help_ = "".join(re.findall(r'TEXT\("((?:[^"\\]|\\.)*)"\)', texts))
            out[name] = dict(type=t, name=name, help=help_)
    return [out[k] for k in sorted(out) if k not in SKIP]


def ident(name):
    s = re.sub(r"[^A-Za-z0-9]", "_", name[3:])
    return "U_" + s


def tip(h):
    h = h.replace('\\"', "'").replace('"', "'").replace("\\n", " ")
    return h


def generate():
    cvs = parse()
    ctype = {"float": "float", "int32": "int32", "FString": "FString"}
    lines = []
    table = []
    order = sorted(cvs, key=lambda c: (CAT_ORDER.index(category(c["name"])[1]), c["name"]))
    for c in order:
        if c["type"] not in ctype:
            continue
        reen = c["name"].startswith(REENABLE_PREFIX) or "uu.Enable" in c["help"] or "reroll" in c["help"].lower()
        meta = 'DisplayName = "%s", UniCVar = "%s"%s, ToolTip = "%s%s"' % (
            c["name"][3:], c["name"], ", UniReenable" if reen else "", tip(c["help"]),
            " [re-enables the scene on change]" if reen else "")
        init = {"float": " = 0.0f", "int32": " = 0", "FString": ""}[c["type"]]
        table.append('\t\t{ TEXT("%s"), TEXT("%s"), %s, TEXT("%s") },' % (ident(c["name"]), c["name"], "true" if reen else "false", category(c["name"])[1]))
        lines.append('\tUPROPERTY(config, EditAnywhere, Category = "%s", meta = (%s))\n\t%s %s%s;\n' % (
            category(c["name"])[1], meta, ctype[c["type"]], ident(c["name"]), init))
    header = '''// UniTuningSettings.h
// GENERATED by Tools/gen_tuning_settings.py from the plugin's uu.* console variables (D-261, B-039):
// do not edit by hand; add or change a console variable and re-run it (T4.static.tuning_settings
// checks this file is current).
//
// Project Settings > Plugins > Unified Universe Tuning: one property per console variable. An edit
// sets the variable as typing it would (live in PIE); a saved value (DefaultGame.ini, "Set as
// Default") is applied when the subsystem starts; a property marked [re-enables] re-enables the
// scene where the camera is, half a second after the last such edit (textures, shaping, anything
// stars are placed from). Console: uu.Tuning.Import (the variables' current values into the page),
// uu.Tuning.Save (the page to DefaultGame.ini), uu.Tuning.Reenable,
// uu.Tuning.SavePreset / LoadPreset <name> (Saved/UniTuningPresets), uu.Tuning.Presets,
// uu.Tuning.Reset [filter] (D-264: the declared defaults back; the page's Tuning buttons do the same).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UniTuningSettings.generated.h"

UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Unified Universe Tuning"))
class UNIFIEDUNIVERSE_API UUniTuningSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUniTuningSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** Every property to its console variable where they differ (start-up; ECVF_SetByProjectSetting,
	 *  so an ini's [ConsoleVariables] or a typed value still wins). */
	void ApplyAll() const;

	/** The console variables' current values into the properties. */
	void ImportAll();

	/** Presets (A/B): every variable on the page with its current value to Saved/UniTuningPresets/<Name>.txt
	 *  (one "uu.Name=value" a line), and back (as typed; the page follows; a changed variable that
	 *  needs it re-enables the scene). */
	static bool SavePreset(const FString& Name);
	static bool LoadPreset(const FString& Name);
	static TArray<FString> ListPresets();
	static FString PresetDir();

	/** D-264: every variable's value when the module loaded (its declared default, or an ini's
	 *  [ConsoleVariables] / the command line's), recorded before the page's saved values are
	 *  applied; a variable not yet registered then is recorded at ApplyAll. */
	static void CaptureDefaults();

	/** D-264: the variables back to those defaults (as typed; the page follows; a changed variable
	 *  that needs it re-enables the scene). Filter (case-insensitive): empty = all; else the variables
	 *  whose name starts with it ("uu.Volume") or whose page category contains it ("Nebulae",
	 *  "March|Steps"). Returns how many changed. DefaultGame.ini is untouched: "Set as Default"
	 *  afterwards makes the reset permanent. */
	static int32 ResetToDefaults(const FString& Filter);

	/** D-264: buttons on the page. */
	UFUNCTION(CallInEditor, Category = "Tuning", meta = (DisplayName = "Reset All To Defaults"))
	void ResetAllToDefaults();
	UFUNCTION(CallInEditor, Category = "Tuning", meta = (DisplayName = "Reset Filtered To Defaults"))
	void ResetFilteredToDefaults();

	/** D-264: what "Reset Filtered To Defaults" resets (not saved). */
	UPROPERTY(Transient, EditAnywhere, Category = "Tuning", meta = (DisplayName = "Reset filter", ToolTip = "What Reset Filtered To Defaults resets: a console variable prefix (uu.Volume, uu.Galaxy.Dust) or a category on this page (Nebulae, March|Steps, Galaxy|Balance). Case-insensitive."))
	FString ResetFilter;

	/** Re-enable the scene when a property marked UniReenable changes (default on). */
	UPROPERTY(config, EditAnywhere, Category = "Tuning", meta = (DisplayName = "Auto re-enable", ToolTip = "Re-enable the scene where the camera is half a second after a change to a setting that needs it (textures, galaxy shaping, populations). Off: type uu.Enable or uu.Tuning.Reenable yourself."))
	bool bAutoReenable = true;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ---- generated: one per console variable ----
''' + "\n".join(lines) + '''};
'''
    cpp = '''// UniTuningSettings.cpp
// GENERATED by Tools/gen_tuning_settings.py (D-261): see UniTuningSettings.h.

#include "UniTuningSettings.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UniSubsystem.h"
#include "UnifiedUniverseModule.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Property, console variable, re-enables, page category (a table: property metadata is editor-only). */
	struct FUniTuningEntry { const TCHAR* Property; const TCHAR* CVar; bool bReenable; const TCHAR* Category; };
	const FUniTuningEntry GUniTuningTable[] = {
''' + "\n".join(table) + '''
	};

	const FUniTuningEntry* UniTuningEntry(const FProperty* P)
	{
		if (!P) { return nullptr; }
		const FString Name = P->GetName();
		for (const FUniTuningEntry& E : GUniTuningTable) { if (Name == E.Property) { return &E; } }
		return nullptr;
	}

	/** The property's value as the console variable's string. */
	bool UniTuningValue(const FProperty* P, const void* Container, FString& Out)
	{
		if (const FFloatProperty* F = CastField<FFloatProperty>(P)) { Out = FString::SanitizeFloat(F->GetPropertyValue_InContainer(Container)); return true; }
		if (const FIntProperty* I = CastField<FIntProperty>(P)) { Out = FString::FromInt(I->GetPropertyValue_InContainer(Container)); return true; }
		if (const FStrProperty* S = CastField<FStrProperty>(P)) { Out = S->GetPropertyValue_InContainer(Container); return true; }
		return false;
	}

	/** D-264: the variables' values when the module loaded (CaptureDefaults). */
	TMap<FString, FString>& UniTuningDefaults()
	{
		static TMap<FString, FString> Defaults;
		return Defaults;
	}

	/** D-264: the page shows values set from code (a reset, a preset): a property change event
	 *  without a property refreshes the details panel (PostEditChangeProperty ignores it). */
	void UniTuningRefreshPage()
	{
#if WITH_EDITOR
		GetMutableDefault<UUniTuningSettings>()->PostEditChange();
#endif
	}

	bool UniTuningSame(const FProperty* P, const void* Container, const IConsoleVariable* V)
	{
		if (const FFloatProperty* F = CastField<FFloatProperty>(P)) { return F->GetPropertyValue_InContainer(Container) == V->GetFloat(); }
		if (const FIntProperty* I = CastField<FIntProperty>(P)) { return I->GetPropertyValue_InContainer(Container) == V->GetInt(); }
		if (const FStrProperty* S = CastField<FStrProperty>(P)) { return S->GetPropertyValue_InContainer(Container) == V->GetString(); }
		return true;
	}
}

UUniTuningSettings::UUniTuningSettings()
{
	// The defaults are the console variables' (one list: their declarations).
	CaptureDefaults();   // D-264: before ApplyAll sets the saved values
	ImportAll();
}

void UUniTuningSettings::ImportAll()
{
	for (const FUniTuningEntry& E : GUniTuningTable)
	{
		FProperty* P = FindFProperty<FProperty>(GetClass(), E.Property);
		const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(E.CVar);
		if (!P || !V) { continue; }
		void* Container = this;
		if (FFloatProperty* F = CastField<FFloatProperty>(P)) { F->SetPropertyValue_InContainer(Container, V->GetFloat()); }
		else if (FIntProperty* I = CastField<FIntProperty>(P)) { I->SetPropertyValue_InContainer(Container, V->GetInt()); }
		else if (FStrProperty* S = CastField<FStrProperty>(P)) { S->SetPropertyValue_InContainer(Container, V->GetString()); }
	}
}

void UUniTuningSettings::CaptureDefaults()
{
	TMap<FString, FString>& D = UniTuningDefaults();
	for (const FUniTuningEntry& E : GUniTuningTable)
	{
		if (D.Contains(E.CVar)) { continue; }
		if (const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(E.CVar)) { D.Add(E.CVar, V->GetString()); }
	}
}

int32 UUniTuningSettings::ResetToDefaults(const FString& Filter)
{
	CaptureDefaults();
	const FString F = Filter.TrimStartAndEnd();
	const TMap<FString, FString>& D = UniTuningDefaults();
	int32 Changed = 0;
	bool bReenable = false;
	for (const FUniTuningEntry& E : GUniTuningTable)
	{
		if (!F.IsEmpty() && !FString(E.CVar).StartsWith(F, ESearchCase::IgnoreCase) && !FString(E.Category).Contains(F, ESearchCase::IgnoreCase)) { continue; }
		const FString* Def = D.Find(E.CVar);
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(E.CVar);
		if (!Def || !V || V->GetString() == *Def) { continue; }
		V->Set(**Def, ECVF_SetByConsole);
		++Changed;
		bReenable = bReenable || E.bReenable;
	}
	GetMutableDefault<UUniTuningSettings>()->ImportAll();
	UniTuningRefreshPage();
	if (bReenable) { UUniSubsystem::RequestReenable(); }
	return Changed;
}

void UUniTuningSettings::ResetAllToDefaults()
{
	const int32 N = ResetToDefaults(FString());
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: %d tuning variables reset to their defaults (Set as Default keeps it)."), N);
}

void UUniTuningSettings::ResetFilteredToDefaults()
{
	const FString F = ResetFilter;   // ResetToDefaults re-imports the page
	const int32 N = F.TrimStartAndEnd().IsEmpty() ? 0 : ResetToDefaults(F);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: %d tuning variables matching '%s' reset to their defaults%s."), N, *F, F.TrimStartAndEnd().IsEmpty() ? TEXT(" (the filter is empty: nothing done; Reset All for everything)") : TEXT(""));
}

void UUniTuningSettings::ApplyAll() const
{
	CaptureDefaults();   // D-264: a variable registered after the page was built
	for (const FUniTuningEntry& E : GUniTuningTable)
	{
		const FProperty* P = FindFProperty<FProperty>(GetClass(), E.Property);
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(E.CVar);
		if (!P || !V || UniTuningSame(P, this, V)) { continue; }
		FString Str;
		if (UniTuningValue(P, this, Str)) { V->Set(*Str, ECVF_SetByProjectSetting); }
	}
}

FString UUniTuningSettings::PresetDir()
{
	return FPaths::ProjectSavedDir() / TEXT("UniTuningPresets");
}

bool UUniTuningSettings::SavePreset(const FString& Name)
{
	if (Name.IsEmpty()) { return false; }
	FString Text;
	for (const FUniTuningEntry& E : GUniTuningTable)
	{
		if (const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(E.CVar)) { Text += FString::Printf(TEXT("%s=%s\\n"), E.CVar, *V->GetString()); }
	}
	return FFileHelper::SaveStringToFile(Text, *(PresetDir() / (Name + TEXT(".txt"))));
}

bool UUniTuningSettings::LoadPreset(const FString& Name)
{
	FString Text;
	if (Name.IsEmpty() || !FFileHelper::LoadFileToString(Text, *(PresetDir() / (Name + TEXT(".txt"))))) { return false; }
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	bool bReenable = false;
	for (const FString& L : Lines)
	{
		FString K, Val;
		if (!L.Split(TEXT("="), &K, &Val)) { continue; }
		K.TrimStartAndEndInline();
		for (const FUniTuningEntry& E : GUniTuningTable)
		{
			if (K != E.CVar) { continue; }
			IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(E.CVar);
			if (V && V->GetString() != Val) { V->Set(*Val, ECVF_SetByConsole); bReenable = bReenable || E.bReenable; }
			break;
		}
	}
	GetMutableDefault<UUniTuningSettings>()->ImportAll();
	UniTuningRefreshPage();   // D-264
	if (bReenable) { UUniSubsystem::RequestReenable(); }
	return true;
}

TArray<FString> UUniTuningSettings::ListPresets()
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(PresetDir() / TEXT("*.txt")), true, false);
	for (FString& F : Files) { F = FPaths::GetBaseFilename(F); }
	return Files;
}

#if WITH_EDITOR
void UUniTuningSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FProperty* P = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty : PropertyChangedEvent.Property;
	const FUniTuningEntry* E = UniTuningEntry(P);
	IConsoleVariable* V = E ? IConsoleManager::Get().FindConsoleVariable(E->CVar) : nullptr;
	if (!V) { return; }
	FString Str;
	if (!UniTuningValue(P, this, Str)) { return; }
	V->Set(*Str, ECVF_SetByConsole);   // as typed: an edit wins over the ini and earlier typing
	if (bAutoReenable && E->bReenable) { UUniSubsystem::RequestReenable(); }
}
#endif
'''
    return header, cpp


def main():
    header, cpp = generate()
    check = "--check" in sys.argv
    bad = []
    for rel, txt in ((HEADER, header), (CPP, cpp)):
        path = os.path.join(PLUGIN, rel)
        cur = open(path).read() if os.path.exists(path) else None
        if cur != txt:
            if check:
                bad.append(rel)
            else:
                open(path, "w").write(txt)
    n = header.count("UniCVar = ")
    if check:
        print(("%d console variables; stale: %s" % (n, ", ".join(bad))) if bad else ("%d console variables; current" % n))
        sys.exit(1 if bad else 0)
    print("%d console variables written" % n)


if __name__ == "__main__":
    main()
