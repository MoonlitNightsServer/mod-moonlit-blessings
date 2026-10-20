// Moonlit Blessings: C++ spell scripts for the Spellblade Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/spellblade.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   The kit
//   spell_spellblade_mana_ward_stance    the stance passive (1440010): keeps the Mana Ward aura on the player while a weapon is held
//   spell_spellblade_mana_ward           the ward (1440011), a Mana Shield aura: takes a share of every hit and the core charges the mana; Incanter's Absorption and Arcane Retort hang here
//   spell_spellblade_arcane_pull         Arcane Pull: bosses are taunted but not pulled
//   spell_spellblade_blink               bound to Blink: Phase Anchor mark, Planar Step, and the "just blinked" effects (Phase Guard, Phase Strike, Weavers' Ward, dispel)
//
//   Boons
//   spell_spellblade_warded_cadence      5% less damage taken and 5% more threat while the mana is above half
//   spell_spellblade_edgebound           melee hits send a share of their damage after them as Arcane damage
//   spell_spellblade_cleaving_arcana     Arcane Strike also hits a second enemy for half
//   spell_spellblade_phase_return        the castable half of Phase Anchor
//   spell_spellblade_blade_dancer        Arcane Strike and Arcane Blast hit every enemy in front
//   spell_spellblade_planar_ready        Planar Step: the "second Blink ready" buff gives the cooldown back when it runs out unused
//
//   Favors
//   spell_spellblade_siphoning_blade     Arcane hits refund mana
//   spell_spellblade_spellstriker        the role flip: teaches the melee Arcane Blast, the Arcane Missiles that is not interrupted, and the melee Mirror Image
//   spell_spellblade_mirror_blades       the Mirror Image aura: every image copies the mage's abilities and (for half) melee swings, like the Dancing Rune Weapon
//   npc_spellblade_mirror_blade          the images' creature AI (a Dancing Rune Weapon clone that follows its owner)
//
// Spell ids come from the Spellblade kit block 1,440,000 and recipe block 1,440,100; tools/talents/recipes/spellblade.py asserts every unit named here.
// A helper spell's id is read from the TriggerSpell column of the passive's dummy effect where the passive has one, otherwise it is the passive's id + 1.
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "GridNotifiers.h"
#include "ObjectAccessor.h"
#include "PassiveAI.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "WorldSession.h"
#include <list>
#include <vector>

namespace
{
    enum SpellbladeSpells : uint32
    {
        SPELL_BLINK                 = 1953,
        SPELL_CLONE_ME              = 45204,     // the owner casts it on an image: the image looks like the owner

        SPELL_ARCANE_BULWARK        = 1440000,   // kit: effect 0 = share of damage the ward takes (80)
        SPELL_MANA_WARD_STANCE      = 1440010,
        SPELL_MANA_WARD             = 1440011,   // kit: effect 0 Mana Shield, effect 1 = base share (40)

        SPELL_WARDED_CADENCE        = 1440100,
        SPELL_EDGEBOUND             = 1440110,
        SPELL_PHASE_GUARD           = 1440120,   // buff = +1
        SPELL_TEMPERING             = 1440150,   // effect 0 = extra points of share (10)
        SPELL_CLEAVING_ARCANA       = 1440160,
        SPELL_PHASE_ANCHOR          = 1440180,   // return spell = +1, mark buff = +2
        SPELL_ARCANE_RETORT         = 1440190,   // effect 0 = percent (25), effect 1 names the bolt
        SPELL_PHASE_STRIKE          = 1440210,   // buff = +1
        SPELL_PLANAR_STEP           = 1440240,   // ready buff = +1
        SPELL_FAVOR_WEAVERS         = 1440260,   // Weavers' Ward buff = +1 (effect 0 = share, 100)
        SPELL_FAVOR_SPELLSTRIKER    = 1440270,   // melee Arcane Blast +1, Arcane Missiles +2, Mirror Image +3
    };

    enum SpellbladeCreatures : uint32
    {
        NPC_MIRROR_BLADE            = 1440280    // creature_template written by tools/phase2/gen_blessings_spells.py
    };

    constexpr int32 WARD_BASE_SHARE_PCT = 40;                  // used only if the ward's own effect cannot be read
    constexpr float CLEAVE_RANGE = 10.0f;                      // yards around the target in which Cleaving Arcana looks for the second enemy
    constexpr float BLADE_RANGE = 8.0f;                        // yards in front of the mage that Blade Dancer reaches
    constexpr float BLADE_ARC = 2.0f * float(M_PI) / 3.0f;     // the width of "in front"
    constexpr uint32 BLADE_MAX_EXTRA = 10;                     // enemies hit besides the target
    constexpr float MARK_SCALE = 10.0f;                        // the Phase Anchor mark keeps positions as tenths of a yard in the amounts of its three dummy effects
    constexpr float RETURN_MAX_DISTANCE = 60.0f;               // Phase Return refuses to jump back to a mark farther than this (a mark from another map)
    constexpr uint32 SPELLSTRIKER_SPELLS[3] = { SPELL_FAVOR_SPELLSTRIKER + 1, SPELL_FAVOR_SPELLSTRIKER + 2, SPELL_FAVOR_SPELLSTRIKER + 3 };   // melee Arcane Blast, Arcane Missiles, Mirror Image
    constexpr uint32 PLANAR_WINDOW_MS = 8 * IN_MILLISECONDS;   // the duration of the Planar Step ready buff, taken off the Blink cooldown if it runs out unused

    // The amount of effect `effect` of an aura of `spellId` on `unit` (0 without the aura).
    int32 Param(Unit const* unit, uint32 spellId, uint8 effect = EFFECT_0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    // Every attackable enemy of `seeker` within `range` yards of `center` that `center` can see.
    std::list<Unit*> EnemiesAround(Unit* seeker, Unit* center, float range)
    {
        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, seeker, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, targets, check);
        Cell::VisitObjects(center, searcher, range);
        targets.remove_if([seeker, center](Unit* unit) { return !seeker->IsValidAttackTarget(unit) || !center->IsWithinLOSInMap(unit); });
        return targets;
    }

    // How much of every hit the Mana Ward takes, in percent: the base share, or the larger share of Arcane Bulwark / the Weavers' Ward while one of them runs, plus Ward Tempering.
    int32 WardSharePct(Unit const* unit, AuraEffect const* base)
    {
        int32 share = base ? base->GetAmount() : WARD_BASE_SHARE_PCT;
        share = std::max(share, Param(unit, SPELL_ARCANE_BULWARK));
        share = std::max(share, Param(unit, SPELL_FAVOR_WEAVERS + 1));
        share += Param(unit, SPELL_TEMPERING);
        return std::min(std::max(share, 0), 100);
    }

    // Removes one harmful magic effect from `unit` (the first it finds).
    void RemoveOneHarmfulMagic(Unit* unit)
    {
        for (auto const& pair : unit->GetAppliedAuras())
        {
            AuraApplication const* application = pair.second;
            Aura const* aura = application->GetBase();
            if (application->IsPositive() || aura->IsPassive() || aura->GetSpellInfo()->Dispel != DISPEL_MAGIC)
                continue;

            unit->RemoveAura(aura->GetId(), aura->GetCasterGUID());
            return;
        }
    }

    // Everything that happens "just after a Blink" (and after Phase Return): Phase Guard, Phase Strike, the Weavers' Ward, and Planar Step's dispel.
    void JustBlinked(Unit* caster)
    {
        if (caster->HasAura(SPELL_PHASE_GUARD))
            caster->CastSpell(caster, SPELL_PHASE_GUARD + 1, true);

        if (caster->HasAura(SPELL_PHASE_STRIKE))
            caster->CastSpell(caster, SPELL_PHASE_STRIKE + 1, true);

        if (caster->HasAura(SPELL_FAVOR_WEAVERS))
            caster->CastSpell(caster, SPELL_FAVOR_WEAVERS + 1, true);

        if (caster->HasAura(SPELL_PLANAR_STEP))
            RemoveOneHarmfulMagic(caster);
    }
}

// The stance: while it is on the player (a weapon is held) the Mana Ward is on the player. The ward is a visible aura that cannot be cancelled.
class spell_spellblade_mana_ward_stance : public AuraScript
{
    PrepareAuraScript(spell_spellblade_mana_ward_stance);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target->HasAura(SPELL_MANA_WARD))
            target->CastSpell(target, SPELL_MANA_WARD, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_MANA_WARD);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_spellblade_mana_ward_stance::HandleApply, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_spellblade_mana_ward_stance::HandleRemove, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The Mana Ward. Effect 0 is a Mana Shield of unlimited size: the core calls OnEffectManaShield for every hit, takes the mana (the effect's value multiplier, 0.5 mana per point of damage,
// lowered by Bottomless Well) and, if the mana runs short, shrinks the absorbed part in the same proportion. Effect 1 holds the base share in percent.
class spell_spellblade_mana_ward : public AuraScript
{
    PrepareAuraScript(spell_spellblade_mana_ward);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        amount = -1;
        canBeRecalculated = false;
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = CalculatePct(dmgInfo.GetDamage(), WardSharePct(GetTarget(), GetAura()->GetEffect(EFFECT_1)));
    }

    // absorbAmount is now what the ward really took (after the mana ran short, if it did)
    void AfterAbsorb(AuraEffect* aurEff, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        if (!absorbAmount)
            return;

        Unit* target = GetTarget();

        // Incanter's Absorption (the stock Boon works on a Mana Shield, and this is one): the same steps as the stock Mana Shield script
        if (AuraEffect* talentAurEff = target->GetAuraEffectOfRankedSpell(44394, EFFECT_0))
        {
            int32 bp = CalculatePct(absorbAmount, talentAurEff->GetAmount());
            if (AuraEffect* currentAura = target->GetAuraEffect(SPELL_AURA_MOD_DAMAGE_DONE, SPELLFAMILY_MAGE, 2941, EFFECT_0))
            {
                bp += int32(currentAura->GetAmount() * (currentAura->GetBase()->GetDuration() / (float)currentAura->GetBase()->GetMaxDuration()));
                currentAura->ChangeAmount(bp);
                currentAura->GetBase()->RefreshDuration();
            }
            else
                target->CastCustomSpell(target, 44413, &bp, nullptr, nullptr, true, nullptr, aurEff);
        }

        // Arcane Retort: the bolt is thrown a moment later, outside the damage that is being worked out right now
        if (AuraEffect const* retort = target->GetAuraEffect(SPELL_ARCANE_RETORT, EFFECT_0))
        {
            Unit* attacker = dmgInfo.GetAttacker();
            int32 const damage = CalculatePct(int32(absorbAmount), retort->GetAmount());
            uint32 const bolt = retort->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
            if (attacker && attacker != target && damage > 0 && bolt)
            {
                ObjectGuid const attackerGuid = attacker->GetGUID();
                target->m_Events.AddEventAtOffset([target, attackerGuid, damage, bolt]()
                {
                    Unit* enemy = ObjectAccessor::GetUnit(*target, attackerGuid);
                    if (enemy && enemy->IsAlive() && target->IsAlive() && target->IsValidAttackTarget(enemy))
                        target->CastCustomSpell(enemy, bolt, &damage, nullptr, nullptr, true);
                }, Milliseconds(1));
            }
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_spellblade_mana_ward::CalculateAmount, EFFECT_0, SPELL_AURA_MANA_SHIELD);
        OnEffectManaShield += AuraEffectManaShieldFn(spell_spellblade_mana_ward::Absorb, EFFECT_0);
        AfterEffectManaShield += AuraEffectManaShieldFn(spell_spellblade_mana_ward::AfterAbsorb, EFFECT_0);
    }
};

// Arcane Pull: effect 2 pulls the target to the mage; bosses keep the taunt but are not pulled.
class spell_spellblade_arcane_pull : public SpellScript
{
    PrepareSpellScript(spell_spellblade_arcane_pull);

    void HandlePull(SpellEffIndex effIndex)
    {
        Unit* hit = GetHitUnit();
        Creature const* creature = hit ? hit->ToCreature() : nullptr;
        if (creature && (creature->IsDungeonBoss() || creature->isWorldBoss() || creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS))
            PreventHitDefaultEffect(effIndex);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_spellblade_arcane_pull::HandlePull, EFFECT_2, SPELL_EFFECT_PULL_TOWARDS);
    }
};

// Blink (1953). Before the cast: Phase Anchor marks where the mage stands (the three dummy effects of the 5 sec mark buff hold x, y and z in tenths of a yard; an aura effect
// whose amount has been set is never recalculated). After the cast (the cooldown has been started by then): Planar Step, and the "just blinked" effects.
class spell_spellblade_blink : public SpellScript
{
    PrepareSpellScript(spell_spellblade_blink);

    void HandleBeforeCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_PHASE_ANCHOR))
            return;

        uint32 const markId = SPELL_PHASE_ANCHOR + 2;
        caster->RemoveAurasDueToSpell(markId);
        if (Aura* mark = caster->AddAura(markId, caster))
        {
            float const position[3] = { caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ() };
            for (uint8 i = 0; i < 3; ++i)
                if (AuraEffect* aurEff = mark->GetEffect(i))
                    aurEff->SetAmount(int32(position[i] * MARK_SCALE));
        }
    }

    void HandleAfterCast()
    {
        Player* caster = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!caster)
            return;

        // Planar Step: the first Blink gives its cooldown back at once and starts the ready buff; the next Blink within 8 sec uses the buff up and starts the cooldown as usual.
        if (caster->HasAura(SPELL_PLANAR_STEP))
        {
            uint32 const ready = SPELL_PLANAR_STEP + 1;
            if (caster->HasAura(ready))
                caster->RemoveAurasDueToSpell(ready);
            else
            {
                caster->RemoveSpellCooldown(SPELL_BLINK, true);
                caster->CastSpell(caster, ready, true);
            }
        }

        JustBlinked(caster);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_spellblade_blink::HandleBeforeCast);
        AfterCast += SpellCastFn(spell_spellblade_blink::HandleAfterCast);
    }
};

// Planar Step: the ready buff. If it runs out unused, Blink's cooldown comes back with the 8 sec that have passed taken off.
class spell_spellblade_planar_ready : public AuraScript
{
    PrepareAuraScript(spell_spellblade_planar_ready);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        Player* player = GetTarget()->ToPlayer();
        SpellInfo const* blink = sSpellMgr->GetSpellInfo(SPELL_BLINK);
        if (!player || !blink)
            return;

        int32 cooldown = int32(blink->CategoryRecoveryTime ? blink->CategoryRecoveryTime : blink->RecoveryTime);
        player->ApplySpellMod(SPELL_BLINK, SPELLMOD_COOLDOWN, cooldown);
        cooldown -= int32(PLANAR_WINDOW_MS);
        if (cooldown > 0)
            player->AddSpellCooldown(SPELL_BLINK, 0, uint32(cooldown), true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_spellblade_planar_ready::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Phase Return: the spell's dummy effect names the mark buff in its TriggerSpell column. The mark holds the position the last Blink started from.
class spell_spellblade_phase_return : public SpellScript
{
    PrepareSpellScript(spell_spellblade_phase_return);

    bool ReadMark(Unit* caster, float& x, float& y, float& z)
    {
        Aura const* mark = caster->GetAura(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
        if (!mark)
            return false;

        AuraEffect const* ex = mark->GetEffect(EFFECT_0);
        AuraEffect const* ey = mark->GetEffect(EFFECT_1);
        AuraEffect const* ez = mark->GetEffect(EFFECT_2);
        if (!ex || !ey || !ez)
            return false;

        x = float(ex->GetAmount()) / MARK_SCALE;
        y = float(ey->GetAmount()) / MARK_SCALE;
        z = float(ez->GetAmount()) / MARK_SCALE;
        return true;
    }

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        float x, y, z;
        if (!ReadMark(caster, x, y, z) || caster->GetExactDist(x, y, z) > RETURN_MAX_DISTANCE)
            return SPELL_FAILED_CASTER_AURASTATE;

        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        float x, y, z;
        if (!ReadMark(caster, x, y, z))
            return;

        caster->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
        caster->RemoveMovementImpairingAuras(true);
        caster->NearTeleportTo(x, y, z, caster->GetOrientation(), true);
        JustBlinked(caster);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_spellblade_phase_return::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_spellblade_phase_return::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Warded Cadence: every second, a visible buff (named in the TriggerSpell column of effect 1) is on while the mana is above half and off otherwise.
class spell_spellblade_warded_cadence : public AuraScript
{
    PrepareAuraScript(spell_spellblade_warded_cadence);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        bool const wanted = target->IsAlive() && uint64(target->GetPower(POWER_MANA)) * 2 > target->GetMaxPower(POWER_MANA);
        if (wanted && !target->HasAura(buff))
            target->CastSpell(target, buff, true);
        else if (!wanted && target->HasAura(buff))
            target->RemoveAurasDueToSpell(buff);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_spellblade_warded_cadence::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Edgebound: the passive's effect 0 is a proc trigger whose value is the percent (10) and whose TriggerSpell is the Arcane bolt; the bolt's damage is its own threat.
class spell_spellblade_edgebound : public AuraScript
{
    PrepareAuraScript(spell_spellblade_edgebound);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim || !victim->IsAlive())
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (damage > 0)
            GetTarget()->CastCustomSpell(victim, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_spellblade_edgebound::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Cleaving Arcana: effect 0 is a proc trigger (value = percent, 50) naming the bolt; another enemy within 10 yards of the target takes it.
class spell_spellblade_cleaving_arcana : public AuraScript
{
    PrepareAuraScript(spell_spellblade_cleaving_arcana);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        Unit* caster = GetTarget();
        Unit* other = caster->SelectNearbyTarget(victim, CLEAVE_RANGE);
        if (!other)
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (damage > 0)
            caster->CastCustomSpell(other, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_spellblade_cleaving_arcana::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Blade Dancer: a hit of Arcane Strike or Arcane Blast (checked by class mask bit below: the proc row's name-based mask is wider) is repeated on up to 10 more enemies in front of the mage within
// 8 yards; every enemy hit, the target included, restores effect 1 percent (1) of the maximum mana.
class spell_spellblade_blade_dancer : public AuraScript
{
    PrepareAuraScript(spell_spellblade_blade_dancer);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        SpellInfo const* spell = eventInfo.GetSpellInfo();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!spell || !damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        // Arcane Blast: word 0 bit 29. Arcane Strike: word 2 bit 30 (the class mask bit the kit gives it).
        if (spell->SpellFamilyName != SPELLFAMILY_MAGE || !spell->SpellFamilyFlags.HasFlag(0x20000000, 0x00000000, 0x40000000))
            return;

        Unit* caster = GetTarget();
        int32 const damage = int32(damageInfo->GetDamage());
        uint32 const bolt = aurEff->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        uint32 hits = 1;
        for (Unit* enemy : EnemiesAround(caster, caster, BLADE_RANGE))
        {
            if (hits > BLADE_MAX_EXTRA)
                break;

            if (enemy == victim || !caster->HasInArc(BLADE_ARC, enemy))
                continue;

            caster->CastCustomSpell(enemy, bolt, &damage, nullptr, nullptr, true, nullptr, aurEff);
            ++hits;
        }

        uint32 const mana = CalculatePct(caster->GetMaxPower(POWER_MANA), std::max(Param(caster, GetId(), EFFECT_1), 0)) * hits;
        caster->EnergizeBySpell(caster, GetId(), mana, POWER_MANA);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_spellblade_blade_dancer::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Favor of the Siphoning Blade: effect 0 is the percent (3) of the damage of every Arcane hit that comes back as mana.
class spell_spellblade_siphoning_blade : public AuraScript
{
    PrepareAuraScript(spell_spellblade_siphoning_blade);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        Unit* target = GetTarget();
        target->EnergizeBySpell(target, GetId(), CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()), POWER_MANA);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_spellblade_siphoning_blade::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Spellstriker: the Favor teaches its three spells and takes them away again when the Favor is dropped (not when the player merely logs out).
// The Lua system removes the tank kit and the spell book's Arcane Blast and Arcane Missiles (blessing_spells.FAVOR_REMOVES).
class spell_spellblade_spellstriker : public AuraScript
{
    PrepareAuraScript(spell_spellblade_spellstriker);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            for (uint32 const spellId : SPELLSTRIKER_SPELLS)
                player->learnSpell(spellId);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        for (uint32 const spellId : SPELLSTRIKER_SPELLS)
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_spellblade_spellstriker::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_spellblade_spellstriker::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Mirror Image (melee): the aura on the mage while the images stand. Adapted from the Dancing Rune Weapon of the Death Knight (spell_dk_dancing_rune_weapon): every ability from the mage's
// spell book that hits an enemy is cast again by each image, and every melee swing is repeated by each image for half its damage.
class spell_spellblade_mirror_blades : public AuraScript
{
    PrepareAuraScript(spell_spellblade_mirror_blades);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* actor = eventInfo.GetActor();
        Unit* target = eventInfo.GetActionTarget();
        if (!actor || !target || !target->IsAlive() || !actor->IsPlayer())
            return false;

        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        if (!spellInfo)
            return true;                                                                // a melee swing

        if (!actor->ToPlayer()->HasActiveSpell(spellInfo->Id))                         // only abilities of the spell book: no procs, helpers or copies
            return false;

        if (spellInfo->IsTargetingArea() && actor != target)                            // an area ability is copied once, around the mage
            return false;

        return !spellInfo->HasEffect(SPELL_EFFECT_SUMMON) && !spellInfo->IsPositive();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* player = eventInfo.GetActor();
        Unit* target = eventInfo.GetActionTarget();
        std::vector<Unit*> images;
        for (Unit* controlled : player->m_Controlled)
            if (controlled && controlled->IsAlive() && controlled->GetEntry() == NPC_MIRROR_BLADE)
                images.push_back(controlled);

        for (Unit* image : images)
        {
            image->SetOrientation(image->GetAngle(target));
            if (SpellInfo const* procSpell = eventInfo.GetSpellInfo())
                image->CastSpell(target, procSpell->Id, true, nullptr, aurEff, image->GetGUID());
            else if (eventInfo.GetDamageInfo())
            {
                Unit* victim = player->GetMeleeHitRedirectTarget(target);
                CalcDamageInfo damageInfo;
                player->CalculateMeleeDamage(victim, &damageInfo, eventInfo.GetDamageInfo()->GetAttackType());
                for (uint8 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
                {
                    Unit::DealDamageMods(victim, damageInfo.damages[i].damage, &damageInfo.damages[i].absorb);
                    damageInfo.damages[i].damage /= 2.0f;
                }

                damageInfo.attacker = image;
                image->SendAttackStateUpdate(&damageInfo);
                image->DealMeleeDamage(&damageInfo, true);
            }
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_spellblade_mirror_blades::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_spellblade_mirror_blades::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// The melee Mirror Images: like the Dancing Rune Weapon they do not act on their own; they stand next to the mage (one on each side), look like the mage (Clone Me) and copy the mage's
// abilities (spell_spellblade_mirror_blades). All their threat goes to the mage.
struct npc_spellblade_mirror_blade : public NullCreatureAI
{
    npc_spellblade_mirror_blade(Creature* creature) : NullCreatureAI(creature) { }

    void InitializeAI() override
    {
        me->AddAura(SPELL_HUNTER_PET_SCALING_04, me);
        if (Unit* owner = me->GetOwner())
        {
            owner->CastSpell(me, SPELL_CLONE_ME, true);
            me->GetThreatMgr().RegisterRedirectThreat(SPELL_FAVOR_SPELLSTRIKER + 3, owner->GetGUID(), 100);

            uint32 neighbours = 0;
            for (Unit* controlled : owner->m_Controlled)
                if (controlled && controlled != me && controlled->GetEntry() == NPC_MIRROR_BLADE)
                    ++neighbours;

            float const angle = neighbours ? 0.5f * float(M_PI) : 1.5f * float(M_PI);
            me->GetMotionMaster()->MoveFollow(owner, 1.0f, angle, MOTION_SLOT_CONTROLLED);
        }

        NullCreatureAI::InitializeAI();
    }
};

void AddMoonlitBlessingsSpellbladeScripts()
{
    RegisterSpellScript(spell_spellblade_mana_ward_stance);
    RegisterSpellScript(spell_spellblade_mana_ward);
    RegisterSpellScript(spell_spellblade_arcane_pull);
    RegisterSpellScript(spell_spellblade_blink);
    RegisterSpellScript(spell_spellblade_planar_ready);
    RegisterSpellScript(spell_spellblade_phase_return);
    RegisterSpellScript(spell_spellblade_warded_cadence);
    RegisterSpellScript(spell_spellblade_edgebound);
    RegisterSpellScript(spell_spellblade_cleaving_arcana);
    RegisterSpellScript(spell_spellblade_blade_dancer);
    RegisterSpellScript(spell_spellblade_siphoning_blade);
    RegisterSpellScript(spell_spellblade_spellstriker);
    RegisterSpellScript(spell_spellblade_mirror_blades);
    RegisterCreatureAI(npc_spellblade_mirror_blade);
}
