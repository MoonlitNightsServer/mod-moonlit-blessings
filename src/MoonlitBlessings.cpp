// Moonlit Blessings (testing stage): the C++ half of the Blessings talent system.
//
// For the classes listed in MoonlitBlessings.Classes the stock ways of gaining talents are closed so that the Blessings system (Lua, mod-ale) is the only route:
//   * learning a talent from the talent window is refused;
//   * trainers show no spells and teach nothing (class spells arrive through Learn Spells, everything else through the Blessings);
//   * on login the stock talents are reset once and free talent points are set to 0.
// Spells granted by the Lua system go through Player::learnSpell and are NOT blocked: this module only uses the talent veto and the trainer hooks.
// ~Moonlit Nights Team

#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "NPCPackets.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Trainer.h"
#include "WorldScript.h"
#include "DatabaseEnv.h"
#include "SpellMgr.h"

#include <set>

namespace
{
    bool IsBlessingsClass(Player const* player)
    {
        if (!player || !sConfigMgr->GetOption<bool>("MoonlitBlessings.Enable", false))
            return false;

        std::string const classes = sConfigMgr->GetOption<std::string>("MoonlitBlessings.Classes", "2,3,5,7,8");
        std::string token;
        for (size_t i = 0; i <= classes.size(); ++i)
        {
            if (i == classes.size() || classes[i] == ',')
            {
                if (!token.empty() && static_cast<uint8>(std::stoul(token)) == player->getClass())
                    return true;
                token.clear();
            }
            else if (classes[i] != ' ')
                token += classes[i];
        }
        return false;
    }
}

class MoonlitBlessingsPlayer : public PlayerScript
{
public:
    MoonlitBlessingsPlayer() : PlayerScript("MoonlitBlessingsPlayer", {
        PLAYERHOOK_CAN_LEARN_TALENT,
        PLAYERHOOK_ON_GET_TRAINER_SPELL_STATE,
        PLAYERHOOK_ON_BEFORE_RECEIVE_SPELL_LIST_FROM_TRAINER,
        PLAYERHOOK_ON_LOGIN
    }) { }

    bool OnPlayerCanLearnTalent(Player* player, TalentEntry const* /*talent*/, uint32 /*rank*/) override
    {
        return !IsBlessingsClass(player);
    }

    void OnPlayerGetTrainerSpellState(Player const* player, uint32 /*trainerId*/, uint32 /*spellId*/, Trainer::SpellState& state) override
    {
        if (IsBlessingsClass(player))
            state = Trainer::SpellState::Unavailable;
    }

    void OnPlayerBeforeReceiveSpellListFromTrainer(Player* player, Creature* /*trainer*/, WorldPackets::NPC::TrainerList& list) override
    {
        if (IsBlessingsClass(player))
            list.Spells.clear();
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsBlessingsClass(player))
            return;

        // The stock talent system is closed for this class: take back anything learned before, once (a no-op when nothing is learned)
        if (player->GetFreeTalentPoints() > 0 || player->GetLevel() >= 10)
        {
            player->resetTalents(true);
            player->SetFreeTalentPoints(0);
        }
    }
};


// At start-up, check that every custom spell the Blessings use (table moonlit_blessing_spell, written by tools/phase2/gen_blessings_spells.py) exists in the spell store.
class MoonlitBlessingsWorld : public WorldScript
{
public:
    MoonlitBlessingsWorld() : WorldScript("MoonlitBlessingsWorld", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        QueryResult result = WorldDatabase.Query("SELECT `spell_id`, `name` FROM `moonlit_blessing_spell`");
        uint32 total = 0, missing = 0;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                ++total;
                if (!sSpellMgr->GetSpellInfo(fields[0].Get<uint32>()))
                {
                    ++missing;
                    LOG_ERROR("server.loading", "Moonlit Blessings: custom spell {} ({}) is not in the spell store", fields[0].Get<uint32>(), fields[1].Get<std::string>());
                }
            } while (result->NextRow());
        }

        LOG_INFO("server.loading", ">> Moonlit Blessings: {} custom spells checked, {} missing", total, missing);
    }
};

void AddMoonlitBlessingsSpellScripts();
void AddMoonlitBlessingsElementalScripts();
void AddMoonlitBlessingsEnhancementScripts();
void AddMoonlitBlessingsRestorationScripts();
void AddMoonlitBlessingsWardenScripts();
void AddMoonlitBlessingsMartyrScripts();
void AddMoonlitBlessingsPenitentScripts();
void AddMoonlitBlessingsShadowScripts();
void AddMoonlitBlessingsVicarScripts();
void AddMoonlitBlessingsHolyScripts();
void AddMoonlitBlessingsInquisitorScripts();
void AddMoonlitBlessingsProtectionScripts();
void AddMoonlitBlessingsRetributionScripts();
void AddMoonlitBlessingsArcaneScripts();
void AddMoonlitBlessingsFireScripts();
void AddMoonlitBlessingsFrostScripts();
void AddMoonlitBlessingsSpellbladeScripts();
void AddMoonlitBlessingsBeastmasteryScripts();
void AddMoonlitBlessingsMarksmanshipScripts();
void AddMoonlitBlessingsSurvivalScripts();
void AddMoonlitBlessingsWildguardScripts();
void AddMoonlitBlessingsAfflictionScripts();
void AddMoonlitBlessingsDemonologyScripts();
void AddMoonlitBlessingsDestructionScripts();
void AddMoonlitBlessingsVoidbinderScripts();
void AddMoonlitBlessingsArmsScripts();
void AddMoonlitBlessingsFuryScripts();
void AddMoonlitBlessingsWarprotectionScripts();
void AddMoonlitBlessingsSkirmisherScripts();
void AddMoonlitBlessingsAssassinationScripts();
void AddMoonlitBlessingsCombatScripts();
void AddMoonlitBlessingsEvaderScripts();
void AddMoonlitBlessingsSubtletyScripts();

void AddSC_moonlit_blessings()
{
    new MoonlitBlessingsPlayer();
    AddMoonlitBlessingsSpellScripts();
    AddMoonlitBlessingsElementalScripts();
    AddMoonlitBlessingsEnhancementScripts();
    AddMoonlitBlessingsRestorationScripts();
    AddMoonlitBlessingsWardenScripts();
    AddMoonlitBlessingsMartyrScripts();
    AddMoonlitBlessingsPenitentScripts();
    AddMoonlitBlessingsShadowScripts();
    AddMoonlitBlessingsVicarScripts();
    AddMoonlitBlessingsHolyScripts();
    AddMoonlitBlessingsInquisitorScripts();
    AddMoonlitBlessingsProtectionScripts();
    AddMoonlitBlessingsRetributionScripts();
    AddMoonlitBlessingsArcaneScripts();
    AddMoonlitBlessingsFireScripts();
    AddMoonlitBlessingsFrostScripts();
    AddMoonlitBlessingsSpellbladeScripts();
    AddMoonlitBlessingsBeastmasteryScripts();
    AddMoonlitBlessingsMarksmanshipScripts();
    AddMoonlitBlessingsSurvivalScripts();
    AddMoonlitBlessingsWildguardScripts();
    AddMoonlitBlessingsAfflictionScripts();
    AddMoonlitBlessingsDemonologyScripts();
    AddMoonlitBlessingsDestructionScripts();
    AddMoonlitBlessingsVoidbinderScripts();
    AddMoonlitBlessingsArmsScripts();
    AddMoonlitBlessingsFuryScripts();
    AddMoonlitBlessingsWarprotectionScripts();
    AddMoonlitBlessingsSkirmisherScripts();
    AddMoonlitBlessingsAssassinationScripts();
    AddMoonlitBlessingsCombatScripts();
    AddMoonlitBlessingsEvaderScripts();
    AddMoonlitBlessingsSubtletyScripts();
    new MoonlitBlessingsWorld();
}
