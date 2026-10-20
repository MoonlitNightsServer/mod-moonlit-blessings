// Moonlit Blessings, Stage 2: the C++ spell scripts of the Martyr (Priest tank).
//
//   spell_blessings_eternal_aegis         the stance (passive): calls and removes the Eternal Aegis shield
//   spell_blessings_eternal_aegis_shield  the shield: a self-only absorb of up to 25% of maximum health that refills constantly, like Stagger
//   spell_blessings_martyrs_mantle        Intellect becomes attack power; armour is multiplied while only cloth is worn
//   spell_blessings_sanctified_blow       the melee strike adds a share of its damage to the shield, like a Blood Shield
//
// Spell ids come from the registry blocks 1,640,000 (Martyr). The rows are in data/sql/db-world/zz_moonlit_blessings_10_spell_dbc.sql.
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

namespace
{
    enum MartyrSpells : uint32
    {
        SPELL_ETERNAL_AEGIS        = 1640010,
        SPELL_ETERNAL_AEGIS_SHIELD = 1640011,
        SPELL_MARTYRS_MANTLE       = 1640012
    };


    constexpr int32 AEGIS_CAP_PCT_OF_HEALTH = 25;          // shield size, percent of maximum health
    constexpr float AEGIS_REFILL_INTERVAL = 4.0f;          // seconds: a self-cast Power Word: Shield on its 4 second cooldown
    constexpr float AEGIS_SPELL_POWER_COEFFICIENT = 0.8068f;
    constexpr int32 SANCTIFIED_BLOW_SHARE_PCT = 10;        // percent of the strike's damage added to the shield
    constexpr int32 ARMOUR_MULTIPLIER_CLOTH_ONLY = 399;    // +400%, stored as value minus one

    int32 AegisCap(Unit const* unit)
    {
        return BlessingsHelpers::Martyr::AegisCap(unit);     // base 25% plus the Boons that raise it
    }

    // Power Word: Shield rank 1 absorbs 44 and rank 10 (level 60) 848: a straight line between them stands in for the shield's size at any level.
    int32 AegisRefillPerSecond(Unit* unit)
    {
        float const level = float(std::max<uint8>(unit->GetLevel(), 1));
        float const base = 44.0f + (level - 1.0f) * (848.0f - 44.0f) / 59.0f;
        float const power = float(std::max<int32>(unit->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_HOLY), 0)) * AEGIS_SPELL_POWER_COEFFICIENT;
        return std::max<int32>(1, int32((base + power) / AEGIS_REFILL_INTERVAL));
    }

    void AddToAegis(Unit* unit, int32 amount)
    {
        if (!unit || amount <= 0)
            return;

        Aura* aura = unit->GetAura(SPELL_ETERNAL_AEGIS_SHIELD);
        if (!aura)
            return;

        if (AuraEffect* effect = aura->GetEffect(EFFECT_0))
            effect->SetAmount(std::min(effect->GetAmount() + amount, AegisCap(unit)));
    }
}

// The stance: while it is known the Aegis shield is up. The third effect is a dummy aura used only to run these handlers.
class spell_blessings_eternal_aegis : public AuraScript
{
    PrepareAuraScript(spell_blessings_eternal_aegis);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target->HasAura(SPELL_ETERNAL_AEGIS_SHIELD))
            target->CastSpell(target, SPELL_ETERNAL_AEGIS_SHIELD, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_ETERNAL_AEGIS_SHIELD);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_eternal_aegis::HandleApply, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_eternal_aegis::HandleRemove, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The shield. It starts full and refills one second at a time, never beyond a quarter of the player's maximum health.
class spell_blessings_eternal_aegis_shield : public AuraScript
{
    PrepareAuraScript(spell_blessings_eternal_aegis_shield);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        if (Unit* caster = GetCaster())
            amount = AegisCap(caster);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        if (AuraEffect* absorb = GetAura()->GetEffect(EFFECT_0))
        {
            int32 const cap = AegisCap(target);
            if (absorb->GetAmount() > cap)
                absorb->SetAmount(cap);
            else if (absorb->GetAmount() < cap)
                absorb->SetAmount(std::min(absorb->GetAmount() + AegisRefillPerSecond(target), cap));
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_eternal_aegis_shield::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_eternal_aegis_shield::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Martyr's Mantle: the armour multiplier only counts while every armour piece worn is cloth.
class spell_blessings_martyrs_mantle : public AuraScript
{
    PrepareAuraScript(spell_blessings_martyrs_mantle);

    static bool OnlyClothWorn(Player const* player)
    {
        static constexpr uint8 slots[] = { EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_WAIST,
                                           EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_WRISTS, EQUIPMENT_SLOT_HANDS };
        for (uint8 const slot : slots)
        {
            Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (proto->Class == ITEM_CLASS_ARMOR && proto->SubClass != ITEM_SUBCLASS_ARMOR_CLOTH && proto->SubClass != ITEM_SUBCLASS_ARMOR_MISC)
                return false;
        }

        return true;
    }

    void CalcArmour(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        Unit* target = GetUnitOwner();
        Player const* player = target ? target->ToPlayer() : nullptr;
        amount = (player && OnlyClothWorn(player)) ? ARMOUR_MULTIPLIER_CLOTH_ONLY : 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_martyrs_mantle::CalcArmour, EFFECT_1, SPELL_AURA_MOD_BASE_RESISTANCE_PCT);
    }
};

// Sanctified Blow: part of the damage dealt goes into the Eternal Aegis.
class spell_blessings_sanctified_blow : public SpellScript
{
    PrepareSpellScript(spell_blessings_sanctified_blow);

    void HandleAfterHit()
    {
        if (Unit* caster = GetCaster())
            AddToAegis(caster, CalculatePct(GetHitDamage(), SANCTIFIED_BLOW_SHARE_PCT));
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_blessings_sanctified_blow::HandleAfterHit);
    }
};

// Favor of the Titan's Maul: the core's "Titan's Grip" switch lets a two-handed weapon share the hands with a shield (the stance requires one).
class spell_blessings_titans_maul : public AuraScript
{
    PrepareAuraScript(spell_blessings_titans_maul);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->SetCanTitanGrip(true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
        {
            player->SetCanTitanGrip(false);
            player->AutoUnequipOffhandIfNeed();
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_titans_maul::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_titans_maul::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Re-run the armour check whenever gear changes.
class MoonlitBlessingsGearScript : public PlayerScript
{
public:
    MoonlitBlessingsGearScript() : PlayerScript("MoonlitBlessingsGearScript", { PLAYERHOOK_ON_EQUIP }) { }

    void OnPlayerEquip(Player* player, Item* /*item*/, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (Aura* aura = player->GetAura(SPELL_MARTYRS_MANTLE))
            aura->RecalculateAmountOfEffects();
    }
};

void AddMoonlitBlessingsSpellScripts()
{
    RegisterSpellScript(spell_blessings_eternal_aegis);
    RegisterSpellScript(spell_blessings_eternal_aegis_shield);
    RegisterSpellScript(spell_blessings_martyrs_mantle);
    RegisterSpellScript(spell_blessings_sanctified_blow);
    RegisterSpellScript(spell_blessings_titans_maul);
    new MoonlitBlessingsGearScript();
}
