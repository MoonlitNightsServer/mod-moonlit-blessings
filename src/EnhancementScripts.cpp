// Moonlit Blessings: C++ spell scripts for the Enhancement Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/enhancement.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_spirit_armor      Boon: while Flurry is on the shaman, a short 5% damage reduction is kept up
//   spell_blessings_stormbringer      Boon: a Stormstrike hit also strikes a second enemy near the shaman for a share of its damage
//   spell_blessings_pack_leader       Boon: the Spirit Wolves share the shaman's Flurry
//   spell_blessings_imbue_keeper      Boon (Stormblessed Weapons) and Favor (Twin Fangs): copies a Shaman weapon imbue to the other hand and, for the Boon, keeps imbues from expiring
//   spell_blessings_greatsword        Favor: teaches Earth Slam while the Favor is chosen
//   MoonlitBlessingsSpiritWolvesScript  Boon (Spirit Wolves' Mending): the wolves' damage heals the owner's most injured ally
//
// The hybrid conversion heals (Vital Strikes, Spirit Healer) use the generic spell_blessings_shaman_hybrid_heal of ElementalScripts.cpp.
// Spell ids come from the Enhancement registry block 1,820,000 (tools/talents/recipes/enhancement.py asserts the units that are named here).
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "WorldSession.h"
#include <vector>

namespace
{
    enum EnhancementSpells : uint32
    {
        SPELL_SPIRIT_WOLVES_MENDING      = 1820000,
        SPELL_SPIRIT_WOLVES_MENDING_HEAL = 1820001,
        SPELL_SPIRIT_ARMOR_BUFF          = 1820031,
        SPELL_STORMBRINGER_STRIKE        = 1820041,
        SPELL_PACK_LEADER_HASTE          = 1820051,
        SPELL_EARTH_SLAM                 = 1820071
    };

    enum EnhancementCreatures : uint32
    {
        NPC_SPIRIT_WOLF = 29264                         // the wolves Feral Spirit summons
    };

    // The value of the imbue script's periodic effect
    enum ImbueModes : int32
    {
        IMBUE_MAIN_TO_OFF         = 1,                  // Favor of Twin Fangs: the main-hand imbue is copied to the off hand
        IMBUE_BOTH_AND_PERMANENT  = 2                   // Stormblessed Weapons: either hand is copied to the other, and imbues never run out
    };

    constexpr uint32 IMBUE_DURATION_MS = 30 * 60 * 1000;    // what Spell::EffectEnchantItemTmp gives a Shaman imbue
    constexpr uint32 IMBUE_REFRESH_TICKS = 5;               // the keeper ticks every 2 sec: refresh the duration every 10 sec
    constexpr float STORMBRINGER_RANGE = 10.0f;             // yards from the shaman within which the second enemy is searched

    // The Flurry buff (all ranks): an attack speed aura of the Shaman family with class mask word 1 bit 9
    AuraEffect const* FindFlurry(Unit const* unit)
    {
        return unit->GetAuraEffect(SPELL_AURA_MOD_MELEE_HASTE, SPELLFAMILY_SHAMAN, 0x00000000, 0x00000200, 0x00000000);
    }

    // Shaman weapon imbues (Windfury, Flametongue, Frostbrand) are temporary enchantments whose enchant refers to a spell of the Shaman family
    bool IsShamanImbue(Item const* item)
    {
        if (!item)
            return false;

        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT));
        if (!enchant)
            return false;

        for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(enchant->spellid[i]))
                if (spellInfo->SpellFamilyName == SPELLFAMILY_SHAMAN)
                    return true;

        return false;
    }

    // Put the imbue of `from` on `to` when `to` is a weapon without an imbue (the same steps as Spell::EffectEnchantItemTmp).
    void CopyImbue(Player* player, Item* from, Item* to)
    {
        if (!from || !to || !IsShamanImbue(from) || to->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return;

        ItemTemplate const* proto = to->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_WEAPON)
            return;

        uint32 const enchantId = from->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchant)
            return;

        to->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchantId, IMBUE_DURATION_MS, enchant->charges, player->GetGUID());
        player->ApplyEnchantment(to, TEMP_ENCHANTMENT_SLOT, true);
    }

    // Start the imbue's 30 minutes again.
    void RefreshImbue(Player* player, Item* item)
    {
        if (!IsShamanImbue(item))
            return;

        player->AddEnchantmentDuration(item, TEMP_ENCHANTMENT_SLOT, IMBUE_DURATION_MS);
        item->SetEnchantmentDuration(TEMP_ENCHANTMENT_SLOT, IMBUE_DURATION_MS, player);
    }
}

// Spirit Armor: while the shaman has Flurry, every second a 2 second buff of 5% less damage taken is renewed.
class spell_blessings_spirit_armor : public AuraScript
{
    PrepareAuraScript(spell_blessings_spirit_armor);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        if (FindFlurry(target))
            target->CastSpell(target, SPELL_SPIRIT_ARMOR_BUFF, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_spirit_armor::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Stormbringer: the strike spell (effect 0 is a proc trigger whose value is the percent) hits another enemy near the shaman.
class spell_blessings_stormbringer : public AuraScript
{
    PrepareAuraScript(spell_blessings_stormbringer);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        Unit* caster = GetTarget();
        Unit* other = caster->SelectNearbyTarget(victim, STORMBRINGER_RANGE);
        if (!other)
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (damage > 0)
            caster->CastCustomSpell(SPELL_STORMBRINGER_STRIKE, SPELLVALUE_BASE_POINT0, damage, other, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_stormbringer::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Pack Leader: the Spirit Wolves get the shaman's Flurry attack speed while the shaman has it (the Feral Spirit count is a plain spell modifier).
class spell_blessings_pack_leader : public AuraScript
{
    PrepareAuraScript(spell_blessings_pack_leader);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        AuraEffect const* flurry = FindFlurry(player);
        if (!flurry)
            return;

        std::vector<Unit*> wolves;
        for (Unit* minion : player->m_Controlled)
            if (minion && minion->IsAlive() && minion->GetEntry() == NPC_SPIRIT_WOLF)
                wolves.push_back(minion);

        int32 const haste = flurry->GetAmount();
        for (Unit* wolf : wolves)
            wolf->CastCustomSpell(SPELL_PACK_LEADER_HASTE, SPELLVALUE_BASE_POINT0, haste, wolf, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_pack_leader::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Stormblessed Weapons and Favor of Twin Fangs: every 2 sec an imbue on one weapon is copied to the other (the periodic effect's value says which way), and for the Boon
// the 30 minutes of every imbue are started again every 10 sec, so they never run out while the Boon is known.
class spell_blessings_imbue_keeper : public AuraScript
{
    PrepareAuraScript(spell_blessings_imbue_keeper);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* offHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        CopyImbue(player, mainHand, offHand);
        if (aurEff->GetAmount() != IMBUE_BOTH_AND_PERMANENT)
            return;

        CopyImbue(player, offHand, mainHand);
        if (_ticks++ % IMBUE_REFRESH_TICKS == 0)
        {
            RefreshImbue(player, mainHand);
            RefreshImbue(player, offHand);
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_imbue_keeper::HandlePeriodic, EFFECT_FIRST_FOUND, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    uint32 _ticks = 0;
};

// Favor of the Greatsword: the Favor aura teaches Earth Slam and takes it away again when the Favor is dropped (not when the player merely logs out).
class spell_blessings_greatsword : public AuraScript
{
    PrepareAuraScript(spell_blessings_greatsword);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->learnSpell(SPELL_EARTH_SLAM);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->removeSpell(SPELL_EARTH_SLAM, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_greatsword::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_greatsword::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Spirit Wolves' Mending: damage dealt by a Spirit Wolf heals the most injured ally near its owner for the percent in the owner's passive (effect 0).
class MoonlitBlessingsSpiritWolvesScript : public UnitScript
{
public:
    MoonlitBlessingsSpiritWolvesScript() : UnitScript("MoonlitBlessingsSpiritWolvesScript", true, { UNITHOOK_ON_DAMAGE }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!attacker || !victim || !damage || !attacker->IsCreature() || attacker->GetEntry() != NPC_SPIRIT_WOLF)
            return;

        Unit* owner = attacker->GetOwner();
        if (!owner || !owner->IsPlayer())
            return;

        AuraEffect const* mending = owner->GetAuraEffect(SPELL_SPIRIT_WOLVES_MENDING, EFFECT_0);
        SpellInfo const* healSpell = sSpellMgr->GetSpellInfo(SPELL_SPIRIT_WOLVES_MENDING_HEAL);
        if (!mending || !healSpell)
            return;

        Unit* ally = BlessingsHelpers::MostInjuredFriend(owner, owner, healSpell->GetMaxRange(true));
        if (!ally)
            return;

        int32 const heal = CalculatePct(int32(std::min(damage, victim->GetHealth())), mending->GetAmount());
        if (heal > 0)
            owner->CastCustomSpell(SPELL_SPIRIT_WOLVES_MENDING_HEAL, SPELLVALUE_BASE_POINT0, heal, ally, true, nullptr, mending);
    }
};

void AddMoonlitBlessingsEnhancementScripts()
{
    RegisterSpellScript(spell_blessings_spirit_armor);
    RegisterSpellScript(spell_blessings_stormbringer);
    RegisterSpellScript(spell_blessings_pack_leader);
    RegisterSpellScript(spell_blessings_imbue_keeper);
    RegisterSpellScript(spell_blessings_greatsword);
    new MoonlitBlessingsSpiritWolvesScript();
}
