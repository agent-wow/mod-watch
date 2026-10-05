/*
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 2 or any later version.
 */

#ifndef MOD_WATCH_STATE_H
#define MOD_WATCH_STATE_H

#include <cstdint>
#include <utility>

namespace Watch
{
constexpr std::uint32_t UpdateInterval = 250;
constexpr std::uint32_t RetryInterval = 2000;

enum class State
{
    Waiting,
    Transferring,
    Watching,
    Paused,
    Stopping
};

enum class Action
{
    None,
    Return,
    Follow,
    Attach,
    Finish
};

struct Observation
{
    bool transferring = false;
    bool targetReady = false;
    bool targetTransferring = false;
    bool destinationAllowed = true;
    bool atHome = false;
    bool sameMap = false;
    bool targetVisible = false;
    bool attached = false;
};

// Kept independent of world objects so the real transition policy can be tested
// without a running realm. The original return snapshot never changes on retarget.
template <class Guid, class Snapshot>
class Assignment
{
public:
    Assignment(Guid target, Snapshot home) : _target(target), _home(std::move(home)) { }

    Guid Target() const { return _target; }
    Snapshot const& Home() const { return _home; }
    State GetState() const { return _state; }
    bool IsStopping() const { return _stopping; }
    bool IsRetrying() const { return _retry != 0; }

    bool ReplaceTarget(Guid target)
    {
        if (_stopping)
            return false;

        _target = target;
        _retry = 0;
        _state = State::Waiting;
        return true;
    }

    void Stop()
    {
        _stopping = true;
        _retry = 0;
        _state = State::Stopping;
    }

    void RetryLater()
    {
        _retry = RetryInterval;
        if (!_stopping)
            _state = State::Paused;
    }

    void Wake() { _retry = 0; }

    void Advance(std::uint32_t diff)
    {
        _retry = diff >= _retry ? 0 : _retry - diff;
    }

    Action Decide(Observation const& observation)
    {
        if (observation.transferring)
        {
            if (!_stopping)
                _state = State::Transferring;
            return Action::None;
        }

        if (_stopping)
            return observation.atHome ? Action::Finish : (_retry ? Action::None : Action::Return);

        if (observation.targetTransferring)
        {
            _state = State::Waiting;
            return Action::None;
        }

        if (!observation.targetReady || !observation.destinationAllowed)
        {
            _state = observation.targetReady ? State::Paused : State::Waiting;
            return observation.atHome || _retry ? Action::None : Action::Return;
        }

        if (_retry)
            return Action::None;

        if (!observation.sameMap || (!observation.attached && !observation.targetVisible))
        {
            _state = State::Transferring;
            return Action::Follow;
        }

        _state = State::Watching;
        return observation.attached ? Action::None : Action::Attach;
    }

private:
    Guid _target;
    Snapshot _home;
    State _state = State::Waiting;
    std::uint32_t _retry = 0;
    bool _stopping = false;
};

struct Binding
{
    std::uint32_t instance = 0;
    bool permanent = false;
    bool extended = false;
};

enum class BindingAction
{
    Keep,
    Remove,
    Restore
};

inline BindingAction RestoreBinding(Binding const& original, Binding const& current,
    std::uint32_t watchedInstance)
{
    // Never undo a reset, an external rebind, or a pre-existing permanent lockout.
    if (!current.instance || current.instance != watchedInstance || original.permanent)
        return BindingAction::Keep;

    if (!original.instance)
        return BindingAction::Remove;

    if (original.instance == current.instance
        && (original.permanent != current.permanent || original.extended != current.extended))
        return BindingAction::Restore;

    return BindingAction::Keep;
}
}

#endif
