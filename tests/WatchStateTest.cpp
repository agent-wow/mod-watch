/*
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 2 or any later version.
 */

#include "WatchCamera.h"
#include "WatchState.h"
#include "WatchTarget.h"
#include "IntegrationTestFixture.h"
#include "SpellAuras.h"
#include "SpellInfoTestHelper.h"
#include <gtest/gtest.h>
#include <string>

namespace
{
using Watch::Action;
using Watch::BindingAction;
using Watch::Observation;
using Watch::State;
using Assignment = Watch::Assignment<unsigned, std::string>;

TEST(WatchTarget, PublicationIsPerObserverAndClearingIsIdempotent)
{
    DataMap first, second;
    ObjectGuid alice = ObjectGuid::Create<HighGuid::Player>(10);
    ObjectGuid bob = ObjectGuid::Create<HighGuid::Player>(20);
    EXPECT_TRUE(Watch::GetTarget(first).IsEmpty());
    Watch::SetTarget(first, alice);
    Watch::SetTarget(second, alice);
    Watch::SetTarget(first, bob);
    EXPECT_EQ(Watch::GetTarget(first), bob);
    EXPECT_EQ(Watch::GetTarget(second), alice);
    Watch::SetTarget(first, ObjectGuid::Empty);
    Watch::SetTarget(first, ObjectGuid::Empty);
    EXPECT_TRUE(Watch::GetTarget(first).IsEmpty());
    EXPECT_EQ(Watch::GetTarget(second), alice);
}

TEST(WatchAssignment, RetargetPreservesOriginalReturnLocation)
{
    Assignment assignment(10, "Stormwind");
    assignment.RetryLater();
    EXPECT_TRUE(assignment.ReplaceTarget(20));
    EXPECT_EQ(assignment.Target(), 20u);
    EXPECT_EQ(assignment.Home(), "Stormwind");
    EXPECT_FALSE(assignment.IsRetrying());
}

TEST(WatchAssignment, OfflineTargetWaitsWithoutLosingAssignment)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.atHome = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    EXPECT_EQ(assignment.GetState(), State::Waiting);
    EXPECT_EQ(assignment.Target(), 10u);

    observation.targetReady = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
}

TEST(WatchAssignment, LogoutReturnsHomeAndReconnectResumes)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetReady = true;
    observation.sameMap = true;
    observation.attached = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    EXPECT_EQ(assignment.GetState(), State::Watching);

    observation.targetReady = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Return);
    observation.transferring = true;
    observation.targetReady = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    observation.transferring = false;
    observation.sameMap = false;
    observation.attached = false;
    observation.atHome = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
    EXPECT_EQ(assignment.Target(), 10u);
}

TEST(WatchAssignment, TargetTransferDoesNotSendObserverHome)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetTransferring = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    EXPECT_EQ(assignment.GetState(), State::Waiting);

    observation.targetTransferring = false;
    observation.targetReady = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
}

TEST(WatchAssignment, RetargetDuringTransferWaitsForAcknowledgement)
{
    Assignment assignment(10, "Stormwind");
    EXPECT_TRUE(assignment.ReplaceTarget(20));
    Observation observation;
    observation.targetReady = true;
    observation.transferring = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    observation.transferring = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
    EXPECT_EQ(assignment.Target(), 20u);
}

TEST(WatchAssignment, StopDuringTransferReturnsBeforeRestoring)
{
    Assignment assignment(10, "Stormwind");
    assignment.Stop();
    Observation observation;
    observation.transferring = true;
    observation.targetReady = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    EXPECT_EQ(assignment.GetState(), State::Stopping);

    observation.transferring = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Return);
    observation.atHome = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Finish);
    EXPECT_FALSE(assignment.ReplaceTarget(20));
    EXPECT_EQ(assignment.Target(), 10u);
}

TEST(WatchAssignment, FailedFollowWaitsForFullRetryInterval)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetReady = true;
    assignment.RetryLater();
    assignment.Advance(Watch::RetryInterval - 1);
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    EXPECT_EQ(assignment.GetState(), State::Paused);
    assignment.Advance(1);
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
}

TEST(WatchAssignment, StopDoesNotWaitForTargetTransfer)
{
    Assignment assignment(10, "Stormwind");
    assignment.Stop();
    Observation observation;
    observation.targetTransferring = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Return);
}

TEST(WatchAssignment, LargeTickDoesNotUnderflowRetryTimer)
{
    Assignment assignment(10, "Stormwind");
    assignment.RetryLater();
    assignment.Advance(60000);
    EXPECT_FALSE(assignment.IsRetrying());
}

TEST(WatchAssignment, StopAndLoginWakeDoNotWaitForOldFailure)
{
    Assignment assignment(10, "Stormwind");
    assignment.RetryLater();
    assignment.Wake();
    EXPECT_FALSE(assignment.IsRetrying());
    assignment.RetryLater();
    assignment.Stop();
    Observation observation;
    EXPECT_EQ(assignment.Decide(observation), Action::Return);
}

TEST(WatchAssignment, ConflictingLockoutPausesAndRecovers)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetReady = true;
    observation.destinationAllowed = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Return);
    EXPECT_EQ(assignment.GetState(), State::Paused);
    observation.atHome = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    observation.destinationAllowed = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
}

TEST(WatchAssignment, CameraCanOnlyAttachAfterArrival)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetReady = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
    observation.transferring = true;
    observation.sameMap = true;
    observation.targetVisible = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    observation.transferring = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Attach);
    observation.attached = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    observation.attached = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Attach);
}

TEST(WatchAssignment, AttachedCameraKeepsBodyInPlaceOutsideBodyVisibility)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetReady = true;
    observation.sameMap = true;
    observation.attached = true;
    observation.targetVisible = false;
    EXPECT_EQ(assignment.Decide(observation), Action::None);
    EXPECT_EQ(assignment.GetState(), State::Watching);

    // If that remote camera is lost, relocate once to acquire it again.
    observation.attached = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
}

TEST(WatchAssignment, VisibleTargetAttachesWithoutMovingBody)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetReady = true;
    observation.sameMap = true;
    observation.targetVisible = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Attach);
    observation.attached = true;
    EXPECT_EQ(assignment.Decide(observation), Action::None);

    // A lost camera can also be restored in place while the target is visible.
    observation.attached = false;
    EXPECT_EQ(assignment.Decide(observation), Action::Attach);
}

TEST(WatchAssignment, MapOrInstanceChangeStillRequiresTransfer)
{
    Assignment assignment(10, "Stormwind");
    Observation observation;
    observation.targetReady = true;
    observation.attached = true;
    observation.targetVisible = true;
    EXPECT_EQ(assignment.Decide(observation), Action::Follow);
    EXPECT_EQ(assignment.GetState(), State::Transferring);
}

TEST(WatchBinding, RemoveOnlyBindingsCreatedByWatchEvenAfterBossKill)
{
    EXPECT_EQ(Watch::RestoreBinding({}, {42, false, false}, 42), BindingAction::Remove);
    EXPECT_EQ(Watch::RestoreBinding({}, {42, true, false}, 42), BindingAction::Remove);
    EXPECT_EQ(Watch::RestoreBinding({}, {99, true, false}, 42), BindingAction::Keep);
}

TEST(WatchBinding, RestoreExistingTemporaryBindingUpgradedByEncounter)
{
    EXPECT_EQ(Watch::RestoreBinding({42, false, false}, {42, true, false}, 42), BindingAction::Restore);
    EXPECT_EQ(Watch::RestoreBinding({42, false, false}, {42, false, false}, 42), BindingAction::Keep);
}

TEST(WatchBinding, PreservePermanentLockoutsExtensionsAndExternalResets)
{
    EXPECT_EQ(Watch::RestoreBinding({42, true, true}, {42, true, true}, 42), BindingAction::Keep);
    EXPECT_EQ(Watch::RestoreBinding({42, false, false}, {}, 42), BindingAction::Keep);
    EXPECT_EQ(Watch::RestoreBinding({42, false, false}, {99, true, false}, 42), BindingAction::Keep);
}

class WatchCameraTest : public IntegrationTestFixture
{
};

TEST_F(WatchCameraTest, HiddenAuraRetainsCoreCorrectionsAndSurvivesDeath)
{
    // Raw DBC data is channeled and non-passive. SpellMgr corrects bind sight
    // before the module creates its private definition.
    TestSpellEntryHelper entry;
    entry.WithId(6277).WithAttributesEx(SPELL_ATTR1_IS_CHANNELED)
        .WithEffect(EFFECT_0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_BIND_SIGHT);
    SpellInfo original(entry.Get());
    original.Attributes |= SPELL_ATTR0_PASSIVE;
    original.AttributesEx &= ~SPELL_ATTR1_IS_CHANNELED;
    original.AttributesEx7 |= SPELL_ATTR7_DISABLE_AURA_WHILE_DEAD;

    auto camera = Watch::CreateCameraSpell(entry.Get(), original);
    TestPlayer* target = CreateTestPlayer();
    Aura* aura = Aura::TryCreate(camera.get(), 1, target, target);
    ASSERT_NE(aura, nullptr);

    // This is the core gate for allocating and sending a visible aura slot.
    EXPECT_FALSE(aura->CanBeSentToClient());
    EXPECT_FALSE(aura->CanBeSaved());
    EXPECT_FALSE(camera->IsChanneled());
    EXPECT_TRUE(camera->HasAttribute(SPELL_ATTR2_ALLOW_DEAD_TARGET));
    target->RemoveAllAurasOnDeath();
    EXPECT_FALSE(aura->IsRemoved());

    // Ordinary .bindsight keeps its existing death behavior.
    EXPECT_FALSE(original.IsDeathPersistent());
    EXPECT_TRUE(original.HasAttribute(SPELL_ATTR7_DISABLE_AURA_WHILE_DEAD));
    aura->Remove();
}
}
