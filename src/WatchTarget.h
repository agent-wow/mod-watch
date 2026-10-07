// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MOD_WATCH_TARGET_H
#define MOD_WATCH_TARGET_H

#include "DataMap.h"
#include "ObjectGuid.h"

namespace Watch
{
// Optional, header-only integration. Access only from the world thread; never
// retain a pointer into CustomData. Camera availability does not end an assignment.
struct TargetData final : DataMap::Base
{
    ObjectGuid Guid;
};

inline ObjectGuid GetTarget(DataMap const& data)
{
    auto target = data.Get<TargetData>("mod-watch.target.v1");
    return target ? target->Guid : ObjectGuid::Empty;
}

inline void SetTarget(DataMap& data, ObjectGuid guid)
{
    if (guid.IsEmpty())
        data.Erase("mod-watch.target.v1");
    else
        data.GetDefault<TargetData>("mod-watch.target.v1")->Guid = guid;
}
}

#endif
