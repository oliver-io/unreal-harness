/**
 * Domain: widget — UMG Widget Blueprint (UWidgetBlueprint) authoring + inspection.
 *
 * Port of the `widget_*` tools in `src/MCP/server.py`. One read tool
 * (widget_tree_read) plus four asset mutators. The mutators recompile +
 * auto-save and are blocked during PIE and from dry_run (per
 * MCPCommonUtils.cpp IsBlockedDuringPie / IsBlockedFromDryRun).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const widgetCreate = bridgeTool({
  name: "widget_create",
  domain: "widget",
  description:
    "Create a new UWidgetBlueprint asset (extends UUserWidget by default). " +
    "Auto-saves; path uniqueness enforced. Specify (path, name) or asset_path. " +
    "Blocked during PIE; dry_run unsupported.",
  input: z.object({
    path: z
      .string()
      .default("")
      .describe('Destination package path ("/Game/UI"). Pair with `name`.'),
    name: z.string().default("").describe('Asset short name ("WBP_HUD").'),
    asset_path: z
      .string()
      .default("")
      .describe(
        'Convenience — combined "/Game/UI/WBP_HUD" replaces (path, name).',
      ),
    parent_class: z
      .string()
      .default("")
      .describe(
        "Optional UUserWidget subclass to extend (script path, asset path — _C " +
          "appended, or short name). Defaults to UUserWidget.",
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  // Omit empty optionals (matches Python's `if path:` guards).
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.parent_class) p.parent_class = a.parent_class;
    return p;
  },
});

const widgetTreeRead = bridgeTool({
  name: "widget_tree_read",
  domain: "widget",
  description:
    "Dump the widget tree of a UWidgetBlueprint — every UWidget with its class, " +
    "parent, and slot configuration. Read-only; never dirties the package.",
  input: z.object({
    widget_path: z
      .string()
      .min(1)
      .describe('Asset path of a UWidgetBlueprint, e.g. "/Game/UI/WBP_HUD".'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const widgetAddChild = bridgeTool({
  name: "widget_add_child",
  domain: "widget",
  description:
    "Add a child widget under a named parent panel in a UWidgetBlueprint. " +
    "Recompiles + auto-saves. Omit parent_name to set the child as the WBP root " +
    "(empty tree only). Blocked during PIE; dry_run unsupported.",
  input: z.object({
    widget_path: z
      .string()
      .min(1)
      .describe('Asset path of a UWidgetBlueprint, e.g. "/Game/UI/WBP_HUD".'),
    child_class: z
      .string()
      .min(1)
      .describe(
        "UWidget subclass to create — script path (/Script/UMG.Button), asset " +
          "path (_C appended), or short name (UButton / Button). Must be concrete.",
      ),
    parent_name: z
      .string()
      .default("")
      .describe(
        "Optional name of an existing UPanelWidget parent. When omitted, the new " +
          "widget becomes the WBP root (valid only for an empty tree).",
      ),
    child_name: z
      .string()
      .default("")
      .describe("Optional name for the new widget. UE auto-disambiguates collisions."),
    is_variable: z
      .boolean()
      .default(true)
      .describe(
        "Expose the widget as a member variable of the generated class (default true) so a graph " +
          "VariableGet resolves to it. Set false only for purely-decorative widgets you never reference.",
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      widget_path: a.widget_path,
      child_class: a.child_class,
      is_variable: a.is_variable,
    };
    if (a.parent_name) p.parent_name = a.parent_name;
    if (a.child_name) p.child_name = a.child_name;
    return p;
  },
});

const widgetBindHandler = bridgeTool({
  name: "widget_bind_handler",
  domain: "widget",
  description:
    "Wire a UFunction handler in a Widget Blueprint to a widget's event delegate " +
    "(e.g. UButton.OnClicked, UEditableTextBox.OnTextChanged). Idempotent: no " +
    "duplicate binding. Recompiles + auto-saves. Blocked during PIE; dry_run unsupported.",
  input: z.object({
    widget_path: z
      .string()
      .min(1)
      .describe('Asset path of a UWidgetBlueprint, e.g. "/Game/UI/WBP_HUD".'),
    widget_name: z
      .string()
      .min(1)
      .describe(
        "Named widget instance inside the WBP (FName, case-sensitive); " +
          "widget_tree_read surfaces them.",
      ),
    event_name: z
      .string()
      .min(1)
      .describe(
        "Multicast delegate property name on the widget (OnClicked, OnTextChanged, " +
          "OnCheckStateChanged, OnSelectionChanged, OnValueChanged, …).",
      ),
  }),
  annotations: {
    idempotentHint: true,
    blockedDuringPie: true,
    dryRunUnsupported: true,
  },
});

const widgetSetProperty = bridgeTool({
  name: "widget_set_property",
  domain: "widget",
  description:
    "Umbrella property setter for UMG widgets — writes any UPROPERTY on the widget " +
    "instance (target='widget') or its UPanelSlot (target='slot') via SetObjectProperty. " +
    "Recompiles + auto-saves; returns before/after exported text. Blocked during PIE; " +
    "dry_run unsupported.",
  input: z.object({
    widget_path: z.string().min(1).describe("Asset path of a UWidgetBlueprint."),
    widget_name: z
      .string()
      .min(1)
      .describe("Named widget instance inside the WBP."),
    property_name: z
      .string()
      .min(1)
      .describe("FProperty name on the target object (case-sensitive)."),
    property_value: z
      .unknown()
      .describe(
        "New value; JSON shape matches the property type — string/number/bool, " +
          "object {…} for FStruct (FLinearColor, FAnchors, FMargin, FSlateBrush), " +
          "array [...] for TArray<>.",
      ),
    target: z
      .enum(["widget", "slot"])
      .default("widget")
      .describe(
        "'widget' (default) sets on the UWidget instance; 'slot' sets on its UPanelSlot.",
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
});

export const widgetTools: ToolDef[] = [
  widgetCreate,
  widgetTreeRead,
  widgetAddChild,
  widgetBindHandler,
  widgetSetProperty,
];
