// Moonlit Blessings: C++ spell scripts for the Balance Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/balance.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_balance_eclipse_dancer   Favor of the Eclipse Dancer: the retired Eclipse talent. A critical Wrath gives a Lunar Eclipse (60%), a critical Starfire a Solar Eclipse (100%), 30 sec
//                                            lockout per Eclipse, none while one is on the druid. Eclipse Sovereign lengthens the Eclipse and adds the cast time helper here
//   spell_blessings_balance_starsurge_adept  Favor of the Starsurge Adept: teaches Starsurge while the Favor lasts (a Favor is only an aura; it cannot teach a spell by itself)
//   spell_blessings_balance_moonbow          Favor of the Moonbow: teaches Auto Shot and the bow, crossbow and gun skills, keeps the ammunition set, keeps one treant at the druid's side while
//                                            a bow, crossbow or gun is wielded, and counts Auto Shots into the Verdant Charge (5 of them make the next Wrath, Starfire or Moonfire instant and free)
//   MoonlitBalanceUnitScript                 UnitScript: Grove Sovereign, every treant of the druid (creature 1964) deals more melee damage
//
// Spell ids: the Balance registry block 1,210,000 and the Druid class-wide kit block 1,240,000 (the units are listed at the top of tools/talents/recipes/balance.py, which asserts the ids named
// below). Amounts and helper spells are read from the effects of the Boon's or Favor's own passive, so the spell data stays the one place a number is written. Every script first checks that the
// druid really has the Boon or Favor aura. A Favor aura is added by the Lua system with AddAura and its scripts run while the aura lives.
// ~Moonlit Nights Team

#include "GameTime.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "UnitScript.h"

#include <algorithm>
#include <vector>

namespace
{
    enum BalanceSpells : uint32
    {
        SPELL_ECLIPSE_SOVEREIGN = 1210000,   // effect 0: amount = extra seconds of every Eclipse; effect 1: amount = percent shorter cast time, trigger spell = the cast time helper
        SPELL_GROVE_SOVEREIGN   = 1210010,   // effect 2: amount = percent more melee damage of every treant
        SPELL_ECLIPSE_DANCER    = 1210030,   // the Favor passive
        SPELL_SOLAR_ECLIPSE     = 1210031,
        SPELL_LUNAR_ECLIPSE     = 1210032,
        SPELL_STARSURGE         = 1240020,   // castable, taught while the Favor of the Starsurge Adept lasts
        SPELL_STARSURGE_EMPOWERED = 1240021, // the one-charge buff Starsurge casts on the druid
        SPELL_AUTO_SHOT         = 75         // stock Auto Shot
    };

    // NPC_TREANT (1964, the stock Treant of Force of Nature) comes from PetDefines.h; the recipe asserts that creature 1964 is what Force of Nature summons
    constexpr uint32 MOONLIT_ARROW = 1000310;         // mod-moonlit-ammo: arrows for bows and crossbows
    constexpr uint32 MOONLIT_SHOT = 1000320;          // mod-moonlit-ammo: bullets for guns
    constexpr uint32 FLAG_WRATH = 0x1;                // SpellFamilyFlags[0] of the Druid spells
    constexpr uint32 FLAG_STARFIRE = 0x4;

    // ---------------------------------------------------------------------------------------------------------------------------------- Eclipse
    // Cast the Solar or the Lunar Eclipse on the druid (15 sec from the spell). With Eclipse Sovereign the Eclipse (and the cast time helper that comes with it) lasts the extra seconds longer.
    void StartEclipse(Unit* druid, bool solar)
    {
        uint32 const eclipseId = solar ? SPELL_SOLAR_ECLIPSE : SPELL_LUNAR_ECLIPSE;
        druid->CastSpell(druid, eclipseId, true);
        Aura* eclipse = druid->GetAura(eclipseId);
        AuraEffect const* sovereign = druid->GetAuraEffect(SPELL_ECLIPSE_SOVEREIGN, EFFECT_0);
        if (!eclipse || !sovereign)
            return;

        int32 const length = eclipse->GetMaxDuration() + std::max(sovereign->GetAmount(), 0) * int32(IN_MILLISECONDS);
        eclipse->SetMaxDuration(length);
        eclipse->SetDuration(length);

        uint32 const helperId = sovereign->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        druid->CastSpell(druid, helperId, true);
        if (Aura* helper = druid->GetAura(helperId))
        {
            helper->SetMaxDuration(length);
            helper->SetDuration(length);
        }
    }

    bool HasEclipse(Unit const* druid)
    {
        return druid->HasAura(SPELL_SOLAR_ECLIPSE) || druid->HasAura(SPELL_LUNAR_ECLIPSE);
    }
}

// Favor of the Eclipse Dancer, the stock Eclipse rules (spell_dru_eclipse). Effect 0 (dummy, proc on a critical Wrath or Starfire): amount = percent chance of the Lunar Eclipse for a Wrath crit,
// trigger spell = the Lunar Eclipse. Effect 1 (dummy): amount = percent chance of the Solar Eclipse for a Starfire crit, trigger spell = the Solar Eclipse. Effect 2 (dummy): amount = seconds
// before the same Eclipse can be gained again. No Eclipse is gained while the druid has one. The lockouts live in the aura, so they restart with the Favor.
class spell_blessings_balance_eclipse_dancer : public AuraScript
{
    PrepareAuraScript(spell_blessings_balance_eclipse_dancer);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        Unit* druid = GetTarget();
        if (!spell || !druid->IsPlayer() || spell->SpellFamilyName != SPELLFAMILY_DRUID || HasEclipse(druid))
            return false;

        bool const wrath = (spell->SpellFamilyFlags[0] & FLAG_WRATH) != 0;
        bool const starfire = (spell->SpellFamilyFlags[0] & FLAG_STARFIRE) != 0;
        if (!wrath && !starfire)
            return false;

        uint32 const now = uint32(GameTime::GetGameTimeMS().count());
        uint32 const lockedUntil = wrath ? _lunarLockedUntil : _solarLockedUntil;
        if (lockedUntil > now)
            return false;

        int32 const chance = GetEffect(wrath ? EFFECT_0 : EFFECT_1)->GetAmount();
        return roll_chance_i(chance);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (!spell)
            return;

        bool const wrath = (spell->SpellFamilyFlags[0] & FLAG_WRATH) != 0;
        uint32 const lockedUntil = uint32(GameTime::GetGameTimeMS().count()) + std::max(GetEffect(EFFECT_2)->GetAmount(), 0) * uint32(IN_MILLISECONDS);
        (wrath ? _lunarLockedUntil : _solarLockedUntil) = lockedUntil;
        StartEclipse(GetTarget(), !wrath);       // a Wrath crit gives the Lunar Eclipse, a Starfire crit the Solar one
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* druid = GetTarget();
        druid->RemoveAurasDueToSpell(SPELL_SOLAR_ECLIPSE);
        druid->RemoveAurasDueToSpell(SPELL_LUNAR_ECLIPSE);
        if (AuraEffect const* sovereign = druid->GetAuraEffect(SPELL_ECLIPSE_SOVEREIGN, EFFECT_0))
            druid->RemoveAurasDueToSpell(sovereign->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_balance_eclipse_dancer::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_balance_eclipse_dancer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_balance_eclipse_dancer::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }

private:
    uint32 _lunarLockedUntil = 0;
    uint32 _solarLockedUntil = 0;
};

// Favor of the Starsurge Adept: the Favor aura teaches Starsurge and takes it away again when the Favor is dropped, not when the player merely logs out.
class spell_blessings_balance_starsurge_adept : public AuraScript
{
    PrepareAuraScript(spell_blessings_balance_starsurge_adept);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (player && player->getClass() == CLASS_DRUID && !player->HasSpell(SPELL_STARSURGE))
            player->learnSpell(SPELL_STARSURGE);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout() || player->getClass() != CLASS_DRUID)
            return;

        player->removeSpell(SPELL_STARSURGE, SPEC_MASK_ALL, false);
        player->RemoveAurasDueToSpell(SPELL_STARSURGE_EMPOWERED);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_balance_starsurge_adept::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_balance_starsurge_adept::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

namespace
{
    // The Moonlit ammunition the ranged weapon in the druid's ranged slot needs: 0 when there is no bow, crossbow or gun there.
    uint32 LauncherAmmo(Player const* player)
    {
        Item const* launcher = player->GetWeaponForAttack(RANGED_ATTACK);
        ItemTemplate const* proto = launcher ? launcher->GetTemplate() : nullptr;
        if (!proto)
            return 0;

        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return MOONLIT_ARROW;
            case ITEM_SUBCLASS_WEAPON_GUN:
                return MOONLIT_SHOT;
            default:
                return 0;
        }
    }

    // The druid has no ammunition slot on the character sheet (the client shows the relic slot instead), so the server keeps the ammunition set: whatever ammunition is set and fits the
    // weapon and is in the bags stays; otherwise one Moonlit Arrow or Moonlit Shot is put in the bags (if there is none) and set. Ammunition is never used up (InfiniteAmmo.Enabled).
    void KeepAmmo(Player* player, uint32 ammoId)
    {
        if (uint32 const current = player->GetUInt32Value(PLAYER_AMMO_ID))
        {
            ItemTemplate const* currentProto = sObjectMgr->GetItemTemplate(current);
            if (currentProto && player->CheckAmmoCompatibility(currentProto) && player->HasItemCount(current))
                return;
        }

        if (!player->HasItemCount(ammoId) && !player->AddItem(ammoId, 1))
            return;

        player->SetAmmo(ammoId);
    }

    // The treant of the Favor: found again through the druid's controlled units and the spell that created it (a Force of Nature treant has another spell). Dead ones are dismissed.
    Creature* FindTreant(Player* player, uint32 summonSpell)
    {
        std::vector<Creature*> dead;
        Creature* alive = nullptr;
        for (Unit* controlled : player->m_Controlled)
        {
            Creature* creature = controlled->ToCreature();
            if (!creature || creature->GetUInt32Value(UNIT_CREATED_BY_SPELL) != summonSpell)
                continue;

            if (creature->IsAlive())
                alive = creature;
            else
                dead.push_back(creature);
        }

        for (Creature* corpse : dead)
            if (TempSummon* summon = corpse->ToTempSummon())
                summon->UnSummon();

        return alive;
    }

    void DismissTreants(Player* player, uint32 summonSpell)
    {
        std::vector<Creature*> treants;
        for (Unit* controlled : player->m_Controlled)
            if (Creature* creature = controlled->ToCreature())
                if (creature->GetUInt32Value(UNIT_CREATED_BY_SPELL) == summonSpell)
                    treants.push_back(creature);

        for (Creature* treant : treants)
            if (TempSummon* summon = treant->ToTempSummon())
                summon->UnSummon();
    }
}

// Favor of the Moonbow. Effect 0 (dummy, proc on a ranged Auto Shot that hits): amount = Auto Shots for a full charge, trigger spell = the counter. Effect 1 (periodic dummy, every 2 sec):
// trigger spell = the treant summon; keeps the treant, the ammunition and Auto Shot in order. Effect 2 (dummy): trigger spell = the ready buff.
class spell_blessings_balance_moonbow : public AuraScript
{
    PrepareAuraScript(spell_blessings_balance_moonbow);

    // The druid has no weapon skill for bows, crossbows or guns, and no Auto Shot: the Favor gives all four while it lasts (the skills are kept when it goes; they do nothing without Auto Shot).
    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || player->getClass() != CLASS_DRUID)
            return;

        uint16 const maxSkill = player->GetMaxSkillValueForLevel();
        for (uint16 const skill : { uint16(SKILL_BOWS), uint16(SKILL_CROSSBOWS), uint16(SKILL_GUNS) })
            if (!player->HasSkill(skill))
                player->SetSkill(skill, 0, maxSkill, maxSkill);

        if (!player->HasSpell(SPELL_AUTO_SHOT))
            player->learnSpell(SPELL_AUTO_SHOT);
    }

    // Dropping the Favor (not logging out): Auto Shot, the treant and the charges go.
    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout() || player->getClass() != CLASS_DRUID)
            return;

        player->removeSpell(SPELL_AUTO_SHOT, SPEC_MASK_ALL, false);
        DismissTreants(player, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
        player->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
        player->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_2].TriggerSpell);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        SpellInfo const* shot = eventInfo.GetSpellInfo();
        if (!player || !shot || shot->Id != SPELL_AUTO_SHOT)
            return;

        // a ready (unspent) surge keeps the counter at zero: the Auto Shots in the meantime are not counted
        uint32 const surge = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell;
        if (player->HasAura(surge))
            return;

        uint32 const counter = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        Aura const* count = player->GetAura(counter);
        if (count && int32(count->GetStackAmount()) + 1 >= std::max(aurEff->GetAmount(), 1))
        {
            player->RemoveAurasDueToSpell(counter);
            player->CastSpell(player, surge, true);
            return;
        }

        player->CastSpell(player, counter, true);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->IsInWorld())
            return;

        uint32 const summonSpell = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        uint32 const ammoId = LauncherAmmo(player);
        if (!ammoId)
        {
            DismissTreants(player, summonSpell);
            return;
        }

        if (!player->IsAlive())
            return;

        if (!player->HasSpell(SPELL_AUTO_SHOT))
            player->learnSpell(SPELL_AUTO_SHOT);

        KeepAmmo(player, ammoId);
        if (!FindTreant(player, summonSpell))
            player->CastSpell(player, summonSpell, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_balance_moonbow::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_balance_moonbow::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectProc += AuraEffectProcFn(spell_blessings_balance_moonbow::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_balance_moonbow::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

class MoonlitBalanceUnitScript : public UnitScript
{
public:
    MoonlitBalanceUnitScript() : UnitScript("MoonlitBalanceUnitScript", true, { UNITHOOK_MODIFY_MELEE_DAMAGE }) { }

    // Grove Sovereign: the melee damage of a treant (creature 1964, the Force of Nature treant and the Moonbow treant alike) whose owner has the Boon. Runs for every melee hit in the world,
    // so the creature entry is the first and cheapest test.
    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        if (!attacker || attacker->GetEntry() != NPC_TREANT || !attacker->IsGuardian())
            return;

        Unit* owner = attacker->GetOwner();
        AuraEffect const* grove = owner && owner->IsPlayer() ? owner->GetAuraEffect(SPELL_GROVE_SOVEREIGN, EFFECT_2) : nullptr;
        if (grove && grove->GetAmount() > 0)
            damage += CalculatePct(damage, grove->GetAmount());
    }
};

void AddMoonlitBlessingsBalanceScripts()
{
    RegisterSpellScript(spell_blessings_balance_eclipse_dancer);
    RegisterSpellScript(spell_blessings_balance_starsurge_adept);
    RegisterSpellScript(spell_blessings_balance_moonbow);
    new MoonlitBalanceUnitScript();
}
