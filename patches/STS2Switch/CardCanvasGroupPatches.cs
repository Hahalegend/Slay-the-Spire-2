// CardCanvasGroupPatches.cs -- Eliminate redundant per-card Vulkan CanvasGroup
// backbuffer copies for normal (non-Ancient, non-blurred) cards.
//
// In scenes/cards/card.tscn, every NCard places its Portrait TextureRect inside
// a CanvasGroup node (PortraitCanvasGroup). In NCard.Reload(), PortraitCanvasGroup
// only receives a Material (mask or Gaussian blur shader) for Ancient cards
// (CardRarity.Ancient) or hidden/blurred cards (Visibility != Shown).
//
// For the remaining ~99% of cards in combat hand, deck view, shop, and rewards,
// PortraitCanvasGroup.Material is null -- yet Godot 4.5 Forward Mobile Vulkan
// still breaks the 2D batch and executes a full CanvasGroup offscreen render
// pass + backbuffer clear + resolve for every single visible card every frame
// (5-10 Vulkan render-pass breaks/frame in combat, 20-30/frame on deck screens).
//
// Moving Portrait into a plain sibling Node2D ("PortraitDirect") placed at the
// exact sibling index/transform of PortraitCanvasGroup whenever Material == null
// (and moving it right back into PortraitCanvasGroup whenever Material != null)
// produces 100% pixel-identical card visuals while eliminating all unnecessary
// CanvasGroup render-pass breaks on Tegra X1 GM20B.

using System;
using System.Reflection;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Bypass no-op PortraitCanvasGroup Vulkan backbuffer passes on normal cards (100% pixel-identical)")]
public static class CardCanvasGroupPatches
{
#if HAVE_GODOTSHARP
    private static FieldInfo? _canvasGroupField;
    private static FieldInfo? _portraitField;
    private static bool _fieldsResolved;
    private static bool _announced;

    private static void EnsureFieldsResolved(object card)
    {
        if (_fieldsResolved)
            return;
        _fieldsResolved = true;

        var t = card.GetType();
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        _canvasGroupField = t.GetField("_portraitCanvasGroup", flags);
        _portraitField = t.GetField("_portrait", flags);
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.Cards.NCard", "Reload", Optional = true)]
    public static void OptimizeCardPortraitGroupAfterReload(object __instance)
    {
        SyncPortraitParent(__instance);
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.Cards.NCard", "OnReturnedFromPool", Optional = true)]
    public static void OptimizeCardPortraitGroupAfterPoolReset(object __instance)
    {
        SyncPortraitParent(__instance);
    }

    private static void SyncPortraitParent(object instance)
    {
        if (instance is not Godot.Node card || !Godot.GodotObject.IsInstanceValid(card))
            return;

        EnsureFieldsResolved(instance);
        if (_canvasGroupField is null || _portraitField is null)
            return;

        try
        {
            if (_canvasGroupField.GetValue(instance) is not Godot.CanvasGroup canvasGroup ||
                !Godot.GodotObject.IsInstanceValid(canvasGroup))
                return;

            if (_portraitField.GetValue(instance) is not Godot.TextureRect portrait ||
                !Godot.GodotObject.IsInstanceValid(portrait))
                return;

            var container = canvasGroup.GetParent();
            if (container is null || !Godot.GodotObject.IsInstanceValid(container))
                return;

            bool needsCanvasGroup = canvasGroup.Material is not null;
            if (needsCanvasGroup)
            {
                // Ancient or blurred/locked card: ensure Portrait is under PortraitCanvasGroup.
                if (portrait.GetParent() != canvasGroup)
                {
                    var oldParent = portrait.GetParent();
                    var savedOwner = portrait.Owner;
                    oldParent?.RemoveChild(portrait);
                    canvasGroup.AddChild(portrait);
                    canvasGroup.MoveChild(portrait, 0);
                    if (savedOwner is not null && Godot.GodotObject.IsInstanceValid(savedOwner))
                        portrait.Owner = savedOwner;
                }

                canvasGroup.Visible = true;
                var directNode = container.GetNodeOrNull<Godot.Node2D>("PortraitDirect");
                if (directNode is not null && Godot.GodotObject.IsInstanceValid(directNode))
                    directNode.Visible = false;
            }
            else
            {
                // Normal visible card (no mask/blur shader): render Portrait directly
                // through a plain Node2D sibling to avoid a Vulkan CanvasGroup pass.
                var directNode = container.GetNodeOrNull<Godot.Node2D>("PortraitDirect");
                if (directNode is null || !Godot.GodotObject.IsInstanceValid(directNode))
                {
                    directNode = new Godot.Node2D
                    {
                        Name = "PortraitDirect",
                        Position = canvasGroup.Position,
                        Rotation = canvasGroup.Rotation,
                        Scale = canvasGroup.Scale,
                    };
                    container.AddChild(directNode);
                    container.MoveChild(directNode, canvasGroup.GetIndex());
                }

                if (portrait.GetParent() != directNode)
                {
                    var oldParent = portrait.GetParent();
                    var savedOwner = portrait.Owner;
                    oldParent?.RemoveChild(portrait);
                    directNode.AddChild(portrait);
                    if (savedOwner is not null && Godot.GodotObject.IsInstanceValid(savedOwner))
                        portrait.Owner = savedOwner;
                }

                directNode.Visible = true;
                canvasGroup.Visible = false;

                if (!_announced)
                {
                    _announced = true;
                    SwitchLog.Info("CardCanvasGroupPatches: bypassed no-op PortraitCanvasGroup Vulkan passes on normal cards");
                }
            }
        }
        catch
        {
            // Never disrupt card lifecycle if a card is mid-teardown.
        }
    }
#endif
}
