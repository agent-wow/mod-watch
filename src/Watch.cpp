/*
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 2 or any later version.
 */

#include "WatchCamera.h"
#include "WatchState.h"
#include "WatchTarget.h"
#include "AllCommandScript.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "InstanceSaveMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ServerScript.h"
#include "SessionScript.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Transport.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Watch
{
enum Spells : uint32
{
    BindSight = 6277
};

struct Snapshot
{
    WorldLocation location;
    uint32 instance;
    uint32 phase;
    Difficulty dungeonDifficulty;
    Difficulty raidDifficulty;
    bool gameMaster;
    bool visible;
    bool spectator;
    bool gravityDisabled;
    bool canFly;
    bool afk;
    uint32 mountSpell = 0;
};

struct TouchedBinding
{
    uint32 map;
    Difficulty difficulty;
    uint32 watchedInstance;
    Binding original;
};

struct Record
{
    Record(ObjectGuid target, Snapshot home, uint32 accountId)
        : assignment(target, std::move(home)), account(accountId) { }

    Assignment<ObjectGuid, Snapshot> assignment;
    uint32 account;
    std::vector<TouchedBinding> bindings;
    ObjectGuid cameraTarget;
    std::string notification;
    std::string restriction;
    bool homeFallback = false;
    bool returning = false;
    bool releaseBinding = false;
};

Binding ReadBinding(InstancePlayerBind const* bind)
{
    return bind && bind->save
        ? Binding{bind->save->GetInstanceId(), bind->perm, bind->extended} : Binding{};
}

class Manager
{
public:
    static Manager& Instance()
    {
        static Manager manager;
        return manager;
    }

    // Packet hooks can execute on map workers. They only read this immutable
    // account set; the records themselves belong exclusively to the world thread.
    bool Guards(WorldSession const* session) const
    {
        return session && _guarded.load()->contains(session->GetAccountId());
    }

    void Configure()
    {
        _enabled = sConfigMgr->GetOption<bool>("Watch.Enable", true);
        if (!_enabled)
            for (auto& [guid, record] : _records)
            {
                record.assignment.Stop();
                if (auto observer = ObjectAccessor::FindConnectedPlayer(guid))
                    SetTarget(observer->CustomData, ObjectGuid::Empty);
            }
    }

    bool Start(ChatHandler* handler, Acore::ChatCommands::PlayerIdentifier const& identifier)
    {
        Player* observer = handler->GetPlayer();
        if (observer->GetSession()->GetSecurity() < SEC_GAMEMASTER)
            return Error(handler, "Watch requires GM security level 2 or higher.");
        if (!_enabled)
            return Error(handler, "Watch is disabled.");
        if (!PrepareCameraSpell())
            return Error(handler, "The bind-sight spell data is unavailable; watch cannot start.");

        ObjectGuid const targetGuid = identifier.GetGUID();
        if (targetGuid == observer->GetGUID())
            return Error(handler, "You cannot watch yourself.");

        if (handler->HasLowerSecurity(identifier.GetConnectedPlayer(), targetGuid))
            return false;

        // Follow GUIDs rather than pointers, including offline watch assignments.
        for (ObjectGuid cursor = targetGuid; _records.contains(cursor);)
        {
            cursor = _records.at(cursor).assignment.Target();
            if (cursor == observer->GetGUID())
                return Error(handler, "That assignment would create a watch cycle.");
        }

        auto existing = _records.find(observer->GetGUID());
        if (existing != _records.end())
        {
            if (existing->second.assignment.IsStopping())
                return Error(handler, "Wait for .unwatch to finish returning you first.");

            Detach(observer, existing->second);
            existing->second.assignment.ReplaceTarget(targetGuid);
            SetTarget(observer->CustomData, targetGuid);
            existing->second.notification.clear();
            handler->PSendSysMessage("Watch target changed to {}.", identifier.GetName());
            return true;
        }

        if (!observer->IsInWorld() || !observer->FindMap() || !observer->IsAlive()
            || observer->IsInCombat() || observer->GetGroup() || observer->GetGroupInvite()
            || observer->IsBeingTeleported() || observer->GetSession()->IsLoggingOut()
            || observer->IsInFlight() || observer->GetVehicle() || observer->GetTransport()
            || observer->InBattleground() || observer->InBattlegroundQueue() || observer->IsSpectator()
            || observer->GetViewpoint() || observer->GetSeer() != observer
            || observer->GetGuidValue(PLAYER_FARSIGHT) || !observer->IsClientControlled(observer)
            || observer->IsRooted()
            || observer->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING)
            || observer->m_mover != observer || observer->duel
            || sLFGMgr->GetState(observer->GetGUID()) != lfg::LFG_STATE_NONE)
        {
            return Error(handler, "Start watching while alive, idle, ungrouped, and outside queues, "
                "battlegrounds, transports, vehicles, taxis, and other remote views.");
        }

        Snapshot snapshot{WorldLocation(observer->GetMapId(), *observer), observer->GetInstanceId(),
            observer->GetPhaseMask(), observer->GetDungeonDifficulty(), observer->GetRaidDifficulty(),
            observer->IsGameMaster(), observer->isGMVisible(), observer->IsGMSpectator(),
            observer->HasUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY), observer->CanFly(), observer->isAFK()};
        auto const& mounts = observer->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        if (!mounts.empty())
            snapshot.mountSpell = mounts.front()->GetSpellInfo()->Id;

        auto itr = _records.try_emplace(observer->GetGUID(), targetGuid, snapshot,
            observer->GetSession()->GetAccountId()).first;
        if (observer->GetMap()->IsDungeon())
            RememberBinding(observer, itr->second, observer->GetMapId(), observer->GetMap()->GetDifficulty(),
                observer->GetInstanceId());
        SetTarget(observer->CustomData, targetGuid);
        PublishGuards();
        // The core uses this if a battleground closes while the GM is inside it.
        observer->SetEntryPoint();
        observer->StopMoving();
        observer->SetGameMaster(true);
        observer->SetGMVisible(false);
        observer->SetGMSpectator(true);
        if (observer->isAFK())
            observer->ToggleAFK();
        Protect(observer);
        observer->GetSession()->ResetTimeOutTime(true);
        handler->PSendSysMessage("Watching {}. Use .unwatch before logging out.", identifier.GetName());
        return true;
    }

    void Stop(Player* observer)
    {
        SetTarget(observer->CustomData, ObjectGuid::Empty);
        auto itr = _records.find(observer->GetGUID());
        if (itr == _records.end())
        {
            ChatHandler(observer->GetSession()).SendSysMessage("You are not watching anyone.");
            return;
        }

        itr->second.assignment.Stop();
        Detach(observer, itr->second);
        Notify(observer, itr->second, "Stopping watch; returning to your starting location.");
    }

    void Login(Player* player)
    {
        for (auto& [guid, record] : _records)
            if (record.assignment.Target() == player->GetGUID())
                record.assignment.Wake();
    }

    void BeforeLogout(Player* player)
    {
        // Called on the world thread, before character saving and deletion.
        SetTarget(player->CustomData, ObjectGuid::Empty);
        for (auto& [guid, record] : _records)
        {
            if (record.assignment.Target() != player->GetGUID())
                continue;

            if (Player* observer = ObjectAccessor::FindConnectedPlayer(guid))
                Detach(observer, record);
            record.assignment.Wake();
        }

        auto itr = _records.find(player->GetGUID());
        if (itr == _records.end())
            return;

        Record& record = itr->second;
        record.assignment.Stop();
        Detach(player, record);
        // LogoutPlayer already completes far transfers. A near transfer needs
        // completing here too; no client acknowledgement is possible on disconnect.
        FinishLogoutTransfer(player);
        RestoreBindings(player, record, false);
        if (!ReturnHome(player, record))
            LOG_ERROR("module.watch", "Could not return observer {} before logout", player->GetGUID().ToString());
        FinishLogoutTransfer(player);
        RestoreBindings(player, record, true);
        RestorePlayer(player, record.assignment.Home());
        _records.erase(itr);
        PublishGuards();
    }

    void Update(uint32 diff)
    {
        _elapsed += diff;
        if (_elapsed < UpdateInterval)
            return;

        uint32 const elapsed = _elapsed;
        _elapsed = 0;
        bool changed = false;
        for (auto itr = _records.begin(); itr != _records.end();)
        {
            Player* observer = ObjectAccessor::FindConnectedPlayer(itr->first);
            if (!observer)
            {
                // Normally removed by BeforeLogout; never retain a dead GUID.
                LOG_ERROR("module.watch", "Observer {} disappeared before watch cleanup", itr->first.ToString());
                itr = _records.erase(itr);
                changed = true;
                continue;
            }

            if (UpdateObserver(observer, itr->second, elapsed))
            {
                itr = _records.erase(itr);
                changed = true;
            }
            else
                ++itr;
        }

        if (changed)
            PublishGuards();
    }

private:
    using Accounts = std::unordered_set<uint32>;

    Manager() : _guarded(std::make_shared<Accounts const>()) { }

    bool PrepareCameraSpell()
    {
        if (_cameraSpell)
            return true;

        SpellEntry const* entry = sSpellStore.LookupEntry(BindSight);
        SpellInfo const* original = sSpellMgr->GetSpellInfo(BindSight);
        if (!entry || !original || !original->HasAura(SPELL_AURA_BIND_SIGHT))
            return false;

        _cameraSpell = CreateCameraSpell(entry, *original);
        return true;
    }

    static bool Error(ChatHandler* handler, char const* message)
    {
        handler->SendErrorMessage(message);
        return false;
    }

    void PublishGuards()
    {
        auto accounts = std::make_shared<Accounts>();
        for (auto const& [guid, record] : _records)
            accounts->insert(record.account);
        _guarded.store(std::move(accounts));
    }

    static void Notify(Player* observer, Record& record, std::string const& message)
    {
        if (record.notification == message)
            return;

        record.notification = message;
        ChatHandler(observer->GetSession()).SendSysMessage(message);
    }

    static void Protect(Player* observer)
    {
        observer->SetClientControl(observer, false);
        observer->SetControlled(true, UNIT_STATE_ROOT);
        observer->SetDisableGravity(true);
        observer->SetCanFly(true);
    }

    static void Detach(Player* observer, Record& record)
    {
        // Removal is caster-scoped: another GM's camera must remain intact.
        if (Player* target = ObjectAccessor::FindConnectedPlayer(record.cameraTarget))
            target->RemoveAurasDueToSpell(BindSight, observer->GetGUID());
        observer->StopCastingBindSight();
        record.cameraTarget.Clear();
    }

    static WorldLocation Home(Player* observer, Record const& record)
    {
        if (!record.homeFallback)
            return record.assignment.Home().location;

        return WorldLocation(observer->m_homebindMapId, observer->m_homebindX, observer->m_homebindY,
            observer->m_homebindZ, record.assignment.Home().location.GetOrientation());
    }

    static bool AtHome(Player* observer, Record const& record)
    {
        WorldLocation const home = Home(observer, record);
        return observer->IsInWorld() && !observer->IsBeingTeleported() && !observer->GetTransport()
            && observer->GetMapId() == home.GetMapId()
            && (record.homeFallback || observer->GetInstanceId() == record.assignment.Home().instance)
            && observer->GetExactDist(&home) < 2.0f
            && std::abs(observer->GetOrientation() - home.GetOrientation()) < 0.01f;
    }

    static void RememberBinding(Player* observer, Record& record, uint32 map, Difficulty difficulty,
        uint32 instance)
    {
        for (TouchedBinding const& binding : record.bindings)
            if (binding.map == map && binding.difficulty == difficulty)
                return;

        record.bindings.push_back({map, difficulty, instance,
            ReadBinding(sInstanceSaveMgr->PlayerGetBoundInstance(observer->GetGUID(), map, difficulty))});
    }

    static void RestoreBindings(Player* observer, Record& record, bool all)
    {
        for (auto itr = record.bindings.begin(); itr != record.bindings.end();)
        {
            if (!all && observer->GetInstanceId() == itr->watchedInstance
                && observer->GetMapId() == itr->map)
            {
                ++itr;
                continue;
            }

            InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(observer->GetGUID(),
                itr->map, itr->difficulty);
            BindingAction const action = RestoreBinding(itr->original, ReadBinding(bind), itr->watchedInstance);
            if (action == BindingAction::Remove)
            {
                sInstanceSaveMgr->PlayerUnbindInstance(observer->GetGUID(), itr->map, itr->difficulty,
                    true, observer);
            }
            else if (action == BindingAction::Restore)
            {
                // Unbind/rebind can destroy the last reference to an InstanceSave.
                // Restore its public binding fields in place and use the core's
                // prepared statements, keeping the save's other players untouched.
                if (bind->perm && !itr->original.permanent)
                    observer->GetSession()->SendCalendarRaidLockout(bind->save, false);
                bind->perm = itr->original.permanent;
                bind->extended = itr->original.extended;

                CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
                auto* update = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_INSTANCE);
                update->SetData(0, itr->original.instance);
                update->SetData(1, itr->original.permanent);
                update->SetData(2, observer->GetGUID().GetCounter());
                update->SetData(3, itr->watchedInstance);
                transaction->Append(update);
                auto* extension = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_INSTANCE_EXTENDED);
                extension->SetData(0, itr->original.extended);
                extension->SetData(1, observer->GetGUID().GetCounter());
                extension->SetData(2, itr->original.instance);
                transaction->Append(extension);
                CharacterDatabase.CommitTransaction(transaction);
            }

            itr = record.bindings.erase(itr);
        }
    }

    static bool DestinationAllowed(Player* observer, Player* target, Record& record)
    {
        record.restriction.clear();
        record.releaseBinding = false;
        if (observer->GetSession()->GetSecurity() < target->GetSession()->GetSecurity())
            record.restriction = "Watch paused: target security is higher than yours.";
        else if (observer->GetGroup())
            record.restriction = "Watch paused: leave your group to continue.";
        else if (target->GetMap()->IsDungeon())
        {
            Difficulty const difficulty = target->GetMap()->GetDifficulty();
            Binding const bind = ReadBinding(sInstanceSaveMgr->PlayerGetBoundInstance(observer->GetGUID(),
                target->GetMapId(), difficulty));
            if (bind.instance && bind.instance != target->GetInstanceId())
            {
                record.restriction = "Watch paused: an existing dungeon binding conflicts with the target's instance.";
                for (TouchedBinding const& touched : record.bindings)
                {
                    if (touched.map == target->GetMapId() && touched.difficulty == difficulty
                        && touched.watchedInstance == bind.instance && !touched.original.instance)
                    {
                        // Leave the old instance before removing our own binding.
                        // An actual pre-watch binding is never replaced this way.
                        record.releaseBinding = true;
                        record.restriction.clear();
                        break;
                    }
                }
            }
            else if (!sInstanceSaveMgr->GetInstanceSave(target->GetInstanceId()))
                record.restriction = "Watch paused: the target's instance is not available.";
        }

        return record.restriction.empty();
    }

    static void LeaveTransport(Player* observer)
    {
        if (Transport* transport = observer->GetTransport())
            transport->RemovePassenger(observer, true);
        observer->SetTransport(nullptr);
        observer->m_movementInfo.transport.Reset();
        observer->RemoveUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    }

    static bool Follow(Player* observer, Player* target, Record& record)
    {
        if (record.releaseBinding)
            return ReturnHome(observer, record);

        Detach(observer, record);
        Map* map = target->GetMap();
        if (map->IsDungeon())
        {
            Difficulty const difficulty = map->GetDifficulty();
            RememberBinding(observer, record, map->GetId(), difficulty, map->GetInstanceId());
            if (!sInstanceSaveMgr->PlayerGetBoundInstance(observer->GetGUID(), map->GetId(), difficulty))
            {
                InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(map->GetInstanceId());
                if (!save)
                    return false;
                sInstanceSaveMgr->PlayerBindToInstance(observer->GetGUID(), save, false);
            }

            if (map->IsRaid())
                observer->SetRaidDifficulty(difficulty);
            else
                observer->SetDungeonDifficulty(difficulty);
        }

        uint32 const previousBattleground = observer->GetBattlegroundId();
        BattlegroundTypeId const previousBattlegroundType = observer->GetBattlegroundTypeId();
        if (map->IsBattlegroundOrArena())
        {
            observer->SetBattlegroundId(target->GetBattlegroundId(), target->GetBattlegroundTypeId(),
                PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
        }

        bool const differentInstance = observer->GetMapId() == target->GetMapId()
            && observer->GetInstanceId() != target->GetInstanceId();
        uint32 options = TELE_TO_GM_MODE;
        LeaveTransport(observer);
        if (Transport* transport = target->GetTransport())
        {
            observer->SetTransport(transport);
            observer->m_movementInfo.transport = target->m_movementInfo.transport;
            observer->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
            transport->AddPassenger(observer, false);
            options |= TELE_TO_NOT_LEAVE_TRANSPORT;
        }

        observer->SetPhaseMask(target->GetPhaseMask(), true);
        record.returning = false;
        bool const result = observer->TeleportTo(target->GetMapId(), target->GetPositionX(),
            target->GetPositionY(), target->GetPositionZ(), target->GetOrientation(), options,
            target, differentInstance);
        if (!result)
        {
            LeaveTransport(observer);
            observer->SetBattlegroundId(previousBattleground, previousBattlegroundType,
                PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
        }
        else if (map->IsBattlegroundOrArena())
        {
            // Leaving a different BG clears the metadata inside TeleportTo.
            // Reapply the destination before the client's worldport ACK selects it.
            observer->SetBattlegroundId(target->GetBattlegroundId(), target->GetBattlegroundTypeId(),
                PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
        }
        return result;
    }

    static bool ReturnHome(Player* observer, Record& record)
    {
        Detach(observer, record);
        Snapshot const& snapshot = record.assignment.Home();
        observer->SetDungeonDifficulty(snapshot.dungeonDifficulty);
        observer->SetRaidDifficulty(snapshot.raidDifficulty);
        if (!record.homeFallback && snapshot.instance)
        {
            Map* map = sMapMgr->FindMap(snapshot.location.GetMapId(), snapshot.instance);
            if (!map || (map->IsDungeon()
                && sInstanceSaveMgr->PlayerGetDestinationInstanceId(observer, map->GetId(), map->GetDifficulty())
                    != snapshot.instance))
            {
                record.homeFallback = true;
                Notify(observer, record, "Your starting instance is unavailable; returning to your homebind.");
            }
        }

        LeaveTransport(observer);
        observer->SetPhaseMask(snapshot.phase, true);
        if (AtHome(observer, record))
        {
            record.returning = false;
            return true;
        }

        WorldLocation home = Home(observer, record);
        bool const differentInstance = !record.homeFallback && observer->GetMapId() == home.GetMapId()
            && observer->GetInstanceId() != snapshot.instance;
        bool result = observer->TeleportTo(home.GetMapId(), home.GetPositionX(), home.GetPositionY(),
            home.GetPositionZ(), home.GetOrientation(), TELE_TO_GM_MODE, nullptr, differentInstance);
        if (!result && !record.homeFallback)
        {
            record.homeFallback = true;
            home = Home(observer, record);
            Notify(observer, record, "Your starting location is unavailable; returning to your homebind.");
            result = observer->TeleportTo(home, TELE_TO_GM_MODE);
        }
        record.returning = result;
        return result;
    }

    bool Attach(Player* observer, Player* target, Record& record)
    {
        Detach(observer, record);
        observer->UpdateVisibilityOf(target);
        if (!observer->HaveAtClient(target))
            return false;

        SpellInfo const* spell = _cameraSpell.get();
        if (!spell)
            return false;

        // Apply only the camera effect. The private definition supports ghosts;
        // the aura still owns all normal leave-world and caster cleanup.
        uint8 effects = 0;
        for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
            if (spell->Effects[i].ApplyAuraName == SPELL_AURA_BIND_SIGHT)
                effects |= 1 << i;
        if (!effects)
            return false;

        Aura* aura = observer->AddAura(spell, effects, target);
        if (!aura)
            return false;

        aura->SetMaxDuration(-1);
        aura->SetDuration(-1);
        record.cameraTarget = target->GetGUID();
        return observer->GetSeer() == target;
    }

    static void RestorePlayer(Player* observer, Snapshot const& snapshot)
    {
        if (!observer->FindMap() || !observer->GetMap()->IsBattlegroundOrArena())
            observer->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE, PLAYER_MAX_BATTLEGROUND_QUEUES,
                false, false, TEAM_NEUTRAL);
        observer->SetControlled(false, UNIT_STATE_ROOT);
        observer->SetClientControl(observer, true);
        observer->SetGMSpectator(snapshot.spectator);
        observer->SetGameMaster(snapshot.gameMaster);
        observer->SetGMVisible(snapshot.visible);
        observer->SetPhaseMask(snapshot.phase, true);
        observer->SetDungeonDifficulty(snapshot.dungeonDifficulty);
        observer->SetRaidDifficulty(snapshot.raidDifficulty);
        // Battleground removal strips mounted auras even from nonparticipants.
        if (snapshot.mountSpell && !observer->HasAura(snapshot.mountSpell))
            observer->CastSpell(observer, snapshot.mountSpell, true);
        observer->SetDisableGravity(snapshot.gravityDisabled);
        observer->SetCanFly(snapshot.canFly);
        if (observer->isAFK() != snapshot.afk)
            observer->ToggleAFK();
        observer->GetSession()->ResetTimeOutTime(true);
    }

    static void FinishLogoutTransfer(Player* observer)
    {
        // This mirrors LogoutPlayer's existing server-side worldport completion.
        while (observer->IsBeingTeleportedFar())
            observer->GetSession()->HandleMoveWorldportAck();

        if (observer->IsBeingTeleportedNear())
        {
            WorldLocation const destination = observer->GetTeleportDest();
            observer->SetSemaphoreTeleportNear(0);
            observer->UpdatePosition(destination, true);
        }
    }

    bool UpdateObserver(Player* observer, Record& record, uint32 diff)
    {
        record.assignment.Advance(diff);
        if (observer->GetSession()->PlayerLogout())
            return false;

        if (!observer->IsBeingTeleported() && observer->IsInWorld())
        {
            RestoreBindings(observer, record, false);
            if (record.returning)
            {
                if (!AtHome(observer, record))
                    record.homeFallback = true;
                record.returning = false;
            }
        }

        if (observer->GetSession()->GetSecurity() < SEC_GAMEMASTER)
        {
            record.assignment.Stop();
            SetTarget(observer->CustomData, ObjectGuid::Empty);
        }

        Player* target = ObjectAccessor::FindConnectedPlayer(record.assignment.Target());
        Observation observation;
        observation.transferring = observer->IsBeingTeleported() || !observer->IsInWorld();
        observation.targetReady = target && target->IsInWorld() && target->FindMap()
            && !target->IsBeingTeleported() && !target->GetSession()->IsLoggingOut();
        observation.targetTransferring = target && !target->GetSession()->IsLoggingOut()
            && (!target->IsInWorld() || target->IsBeingTeleported());
        observation.atHome = AtHome(observer, record);
        if (observation.targetReady && !observation.transferring)
        {
            observation.destinationAllowed = DestinationAllowed(observer, target, record);
            observation.sameMap = observer->GetMap() == target->GetMap();
            observation.attached = observer->GetSeer() == target;
            if (observation.sameMap && observation.destinationAllowed && !record.assignment.IsStopping())
            {
                if (observer->GetPhaseMask() != target->GetPhaseMask())
                    observer->SetPhaseMask(target->GetPhaseMask(), true);
                // Without a camera, use normal client visibility to decide whether
                // reattachment needs a teleport. Distance and transport changes
                // alone must never move an observer with a working camera.
                observation.targetVisible = observer->CanSeeOrDetect(target, false, true);
            }
        }

        Action const action = record.assignment.Decide(observation);
        if (!observation.transferring && (!observation.targetReady || !observation.destinationAllowed))
            Detach(observer, record);

        if (action == Action::None && record.assignment.IsRetrying())
            return false;

        if (action == Action::Finish)
        {
            Detach(observer, record);
            RestoreBindings(observer, record, true);
            RestorePlayer(observer, record.assignment.Home());
            ChatHandler(observer->GetSession()).SendSysMessage("Watch ended; your settings have been restored.");
            return true;
        }

        if (action == Action::Return)
        {
            if (!ReturnHome(observer, record))
            {
                record.assignment.RetryLater();
                Notify(observer, record, "Return teleport failed; retrying. Watch protection remains active.");
                return false;
            }
        }
        else if (action == Action::Follow)
        {
            if (!Follow(observer, target, record))
            {
                record.assignment.RetryLater();
                Notify(observer, record, "Watch paused: teleport failed; retrying.");
                return false;
            }
        }
        else if (action == Action::Attach)
        {
            if (target->GetMap()->IsDungeon())
                RememberBinding(observer, record, target->GetMapId(), target->GetMap()->GetDifficulty(),
                    target->GetInstanceId());
            Protect(observer);
            observer->SetPhaseMask(target->GetPhaseMask(), true);
            if (!Attach(observer, target, record))
            {
                record.assignment.RetryLater();
                Notify(observer, record, "Watch paused: camera is not available yet; retrying.");
                return false;
            }
        }
        switch (record.assignment.GetState())
        {
            case State::Waiting:
                Notify(observer, record, "Waiting for target login or transfer; watch will resume automatically.");
                break;
            case State::Paused:
                Notify(observer, record, record.restriction);
                break;
            case State::Transferring:
                Notify(observer, record, "Following the target; waiting for transfer completion.");
                break;
            case State::Watching:
                Notify(observer, record, "Watching the target. Use .unwatch to stop.");
                break;
            case State::Stopping:
                break;
        }
        return false;
    }

    std::unique_ptr<SpellInfo> _cameraSpell;
    std::unordered_map<ObjectGuid, Record> _records;
    std::atomic<std::shared_ptr<Accounts const>> _guarded;
    uint32 _elapsed = 0;
    bool _enabled = true;
};

bool RejectGameplay(Player* player)
{
    if (!Manager::Instance().Guards(player->GetSession()))
        return false;

    ChatHandler(player->GetSession()).SendSysMessage("Use .unwatch before joining gameplay.");
    return true;
}

class WatchCommands final : public CommandScript
{
public:
    WatchCommands() : CommandScript("mod_watch_commands") { }

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        using namespace Acore::ChatCommands;
        static ChatCommandTable commands =
        {
            {"watch", HandleWatch, SEC_GAMEMASTER, Console::No},
            {"unwatch", HandleUnwatch, SEC_GAMEMASTER, Console::No}
        };
        return commands;
    }

    static bool HandleWatch(ChatHandler* handler, Acore::ChatCommands::PlayerIdentifier target)
    {
        return Manager::Instance().Start(handler, target);
    }

    static bool HandleUnwatch(ChatHandler* handler)
    {
        Manager::Instance().Stop(handler->GetPlayer());
        return true;
    }
};

class WatchWorld final : public WorldScript
{
public:
    WatchWorld() : WorldScript("mod_watch_world", {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE}) { }

    void OnAfterConfigLoad(bool /*reload*/) override { Manager::Instance().Configure(); }
    void OnUpdate(uint32 diff) override { Manager::Instance().Update(diff); }
};

class WatchPlayers final : public PlayerScript
{
public:
    WatchPlayers() : PlayerScript("mod_watch_players", {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_BEFORE_LOGOUT,
        PLAYERHOOK_CAN_GROUP_INVITE, PLAYERHOOK_CAN_GROUP_ACCEPT, PLAYERHOOK_CAN_JOIN_IN_BATTLEGROUND_QUEUE,
        PLAYERHOOK_CAN_JOIN_IN_ARENA_QUEUE, PLAYERHOOK_CAN_JOIN_LFG, PLAYERHOOK_CAN_INIT_TRADE}) { }

    void OnPlayerLogin(Player* player) override { Manager::Instance().Login(player); }
    void OnPlayerBeforeLogout(Player* player) override { Manager::Instance().BeforeLogout(player); }
    bool OnPlayerCanGroupInvite(Player* player, std::string& /*name*/) override { return !RejectGameplay(player); }
    bool OnPlayerCanGroupAccept(Player* player, Group* /*group*/) override { return !RejectGameplay(player); }
    bool OnPlayerCanJoinLfg(Player* player, uint8 /*roles*/, std::set<uint32>& /*dungeons*/,
        std::string const& /*comment*/) override { return !RejectGameplay(player); }

    bool OnPlayerCanJoinInBattlegroundQueue(Player* player, ObjectGuid /*battlemaster*/, BattlegroundTypeId /*type*/,
        uint8 /*asGroup*/, GroupJoinBattlegroundResult& /*error*/) override { return !RejectGameplay(player); }

    bool OnPlayerCanJoinInArenaQueue(Player* player, ObjectGuid /*battlemaster*/, uint8 /*slot*/,
        BattlegroundTypeId /*type*/, uint8 /*asGroup*/, uint8 /*rated*/,
        GroupJoinBattlegroundResult& /*error*/) override { return !RejectGameplay(player); }

    bool OnPlayerCanInitTrade(Player* player, Player* target) override
    {
        return !RejectGameplay(player) && (!target || !Manager::Instance().Guards(target->GetSession()));
    }
};

class WatchSessions final : public SessionScript
{
public:
    WatchSessions() : SessionScript("mod_watch_sessions", {SESSIONHOOK_ON_UPDATE}) { }

    void OnSessionUpdate(WorldSession* session, uint32 /*diff*/) override
    {
        if (Manager::Instance().Guards(session))
            session->ResetTimeOutTime(true);
    }
};

class WatchCommandGuard final : public AllCommandScript
{
public:
    WatchCommandGuard() : AllCommandScript("mod_watch_command_guard", {ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND}) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view command) override
    {
        if (!Manager::Instance().Guards(handler.GetSession()))
            return true;

        std::string_view const name = command.substr(0, command.find(' '));
        if (name == "watch" || name == "unwatch" || name == "help")
            return true;

        handler.SendErrorMessage("Use .unwatch before other GM commands. Spell out .watch and .unwatch in full.");
        return false;
    }
};

class WatchPackets final : public ServerScript
{
public:
    WatchPackets() : ServerScript("mod_watch_packets", {SERVERHOOK_CAN_PACKET_RECEIVE}) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (!Manager::Instance().Guards(session))
            return true;

        switch (packet.GetOpcode())
        {
            case CMSG_LOGOUT_REQUEST:
            {
                WorldPackets::Character::LogoutResponse response;
                response.LogoutResult = 2;
                session->SendPacket(response.Write());
                ChatHandler(session).SendSysMessage("Watch is active. Use .unwatch before logging out.");
                return false;
            }
            case CMSG_MESSAGECHAT:
                // Inspect the type without changing the packet's read cursor.
                return packet.size() < sizeof(uint32) || packet.read<uint32>(0) != CHAT_MSG_AFK;
            case CMSG_CAST_SPELL:
            case CMSG_USE_ITEM:
            case CMSG_ATTACKSWING:
            case CMSG_PET_ACTION:
            case CMSG_GAMEOBJ_USE:
            case CMSG_GAMEOBJ_REPORT_USE:
            case CMSG_GOSSIP_HELLO:
            case CMSG_GOSSIP_SELECT_OPTION:
            case CMSG_LOOT:
            case CMSG_SPELLCLICK:
            case CMSG_CANCEL_AURA:
            case CMSG_TOGGLE_PVP:
            case MSG_MOVE_START_FORWARD:
            case MSG_MOVE_START_BACKWARD:
            case MSG_MOVE_STOP:
            case MSG_MOVE_START_STRAFE_LEFT:
            case MSG_MOVE_START_STRAFE_RIGHT:
            case MSG_MOVE_STOP_STRAFE:
            case MSG_MOVE_JUMP:
            case MSG_MOVE_START_TURN_LEFT:
            case MSG_MOVE_START_TURN_RIGHT:
            case MSG_MOVE_STOP_TURN:
            case MSG_MOVE_START_PITCH_UP:
            case MSG_MOVE_START_PITCH_DOWN:
            case MSG_MOVE_STOP_PITCH:
            case MSG_MOVE_SET_RUN_MODE:
            case MSG_MOVE_SET_WALK_MODE:
            case MSG_MOVE_FALL_LAND:
            case MSG_MOVE_START_SWIM:
            case MSG_MOVE_STOP_SWIM:
            case MSG_MOVE_SET_FACING:
            case MSG_MOVE_SET_PITCH:
            case MSG_MOVE_HEARTBEAT:
            case CMSG_MOVE_FALL_RESET:
            case CMSG_MOVE_SET_FLY:
            case MSG_MOVE_START_ASCEND:
            case MSG_MOVE_STOP_ASCEND:
            case CMSG_MOVE_CHNG_TRANSPORT:
            case MSG_MOVE_START_DESCEND:
                return false;
            default:
                // Teleport, movement-order, time-sync and network acknowledgements
                // must still reach the core; dropping them stalls transfers.
                return true;
        }
    }
};
}

void AddWatchScripts()
{
    new Watch::WatchCommands();
    new Watch::WatchWorld();
    new Watch::WatchPlayers();
    new Watch::WatchSessions();
    new Watch::WatchCommandGuard();
    new Watch::WatchPackets();
}
