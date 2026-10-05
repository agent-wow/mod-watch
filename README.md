# mod-watch

GM camera watching for AzerothCore 3.3.5a. Uses existing module hooks; no core patch,
client addon, custom database table, or SQL installation is required.

## Installation

Place this directory at `modules/mod-watch`, then configure and build AzerothCore
with modules enabled using your normal deployment workflow. The loader is
`Addmod_watchScripts`. The core discovers `src/` and installs
`conf/mod_watch.conf.dist` automatically. Copy the installed template to
`mod_watch.conf` if you want to override its default:

```ini
Watch.Enable = 1
```

Reloading configuration with `Watch.Enable = 0` stops all assignments and returns
their observers. Existing transfers finish before cleanup.

The parent AzerothCore repository ignores external module directories by default.
Keep this directory in a separate module repository or explicitly include it in
your deployment source archive.

## Commands

| Command | Behavior |
| --- | --- |
| `.watch <player name>` | Start watching an existing character, including one currently offline. |
| `.watch <another name>` | Replace the target while retaining the original return location. |
| `.unwatch` | Stop, return, and restore the GM's previous settings. Safe to repeat. |

Security level 2 (`SEC_GAMEMASTER`) or higher is required. Normal target security
checks apply. A failed command does not replace an existing assignment. Multiple
GMs may watch the same character; self-watching and assignment cycles are rejected.

Start alive, out of combat, ungrouped, and outside battlegrounds, queues, taxis,
vehicles, transports, other remote views, and pending teleports/logouts. Finish any
pending group invitation or crowd control first. The module temporarily enables
GM mode, GM invisibility, and GM spectator protection, disables movement, and
prevents falling. The camera follows the target; it does not control the target.

While watching, normal logout requests are rejected, even when the target is
offline. Use `.unwatch` first. AFK requests are suppressed and the observer's
session inactivity timer is refreshed. Closing the client, losing the connection,
administrative kicks, and server shutdown still use normal logout cleanup.

Group/queue entry and gameplay actions are blocked during observation. Other GM
commands require `.unwatch` first; `.help` remains available. Spell out `.watch`
and `.unwatch` in full while an assignment is active.

## Following and restoration

- Assignments use character GUIDs, so target logout/login and renaming do not lose
  the assignment. The GM's logout ends it; assignments are not stored in a database.
- The world thread reconciles assignments every 250 ms. Like `.bindsight`, a
  working camera leaves the hidden body in place, regardless of distance or the
  target boarding/leaving a transport. A visible target can be attached without
  moving. The body relocates only to enter the target's map/instance or to restore
  a lost camera when the target is no longer visible from the observer's position.
  Failed teleports or camera attachment retry after two seconds; loading screens
  are allowed to complete before another transfer. GM mode, invisibility, and
  movement/falling protection remain active even while the body stays behind.
- Continents, phases, dungeons, raids, battlegrounds, arenas, moving transports,
  target taxi flights, vehicles, death, and ghost release use the same follow loop.
  Battleground/arena entry does not enroll the GM as a participant.
  The core's battleground return point is set when watching starts, so match
  shutdowns can eject the observer normally.
- Target logout returns the GM to the starting location while retaining watch
  protection. A target in transit is allowed to finish transferring. The camera
  resumes automatically once the target is available.
- A conflicting pre-existing dungeon binding pauses observation. When no binding
  exists, the module borrows the target's instance through the core's normal
  binding API. These operations write normal character-instance records.
- Module-created bindings are removed after leaving. Existing temporary bindings
  upgraded by an encounter are restored; pre-existing permanent lockouts remain.
  Moving between two instances of the same dungeon may briefly return the GM home
  to release a watch-created binding first.
- `.unwatch` returns to the saved position and restores phase, difficulty, GM
  visibility/mode, spectator, mount, flight, gravity, control, and initial AFK status. If
  the starting instance/location is unavailable, it uses the homebind instead.
- Cleanup runs before character saving on disconnection and normal shutdown.
  Abrupt process crashes cannot restore the in-memory snapshot: temporary GM
  settings, positions, or bindings may have been saved by the core. Inspect the
  observer character before reusing it after a crash.

This is a server-side camera using the existing bind-sight effect. Map loading
still produces loading screens; watching is not a recording or a reproduction of
the target's UI. Disabled maps and restrictions imposed by other modules can pause
the watch. No changes are made to the server-wide AFK logout configuration or to
account flags. Command help is documented here; without `command` table help rows
the core may log its standard missing-help notice for these two commands.

The module owns a private definition of spell 6277 for its camera auras, preserving
the core's passive, hidden bind-sight behavior while allowing dead targets and
preventing aura persistence. Watching does not add a visible buff/debuff icon to
the target. The shared spell definition and ordinary `.bindsight` are unchanged.

## Validation

`mod-watch.cmake` registers `tests/WatchStateTest.cpp` with the existing
`BUILD_TESTING` Google Test target. Test sources stay outside `src/`, so normal
module builds do not require Google Test. After explicitly configuring/building
tests, run the unit-test executable with `--gtest_filter='Watch*'`.

The tests exercise the production assignment/transition policy, binding restoration
decisions, and the camera aura's visibility/death behavior through core aura APIs.
They do not replace client validation of camera packets, transport handling, or
long sessions.

Before deploying, use two 3.3.5a clients and a dedicated GM character to verify:

1. Start against online/offline targets; reconnect and rename the target; switch
   targets; reject unknown names, self-watching, security violations, and cycles.
2. Cross continents, dungeon copies/difficulties, raids, phases, battlegrounds,
   arenas, boats/zeppelins, taxi routes, and vehicles. Kill/release/resurrect the
   target. Verify arrival in the correct instance and no battleground enrollment.
3. Start with no dungeon binding, an existing temporary binding, an existing
   permanent binding, and a conflicting binding. Kill a raid boss while watched,
   then verify only watch-induced binding changes are removed/restored.
4. Use multiple observers; stop one and confirm the others retain their cameras.
   Switch targets or stop during loading and rapidly reconnect the target.
5. Disconnect the GM, disable the module on config reload, and shut down normally.
   Verify saved location/settings and no remaining bind-sight references.
6. Leave a client idle for several hours, both watching and waiting offline. Cycle
   the target through logout/login and battlegrounds. Confirm no AFK logout/exit,
   then `.unwatch` and verify that normal movement and logout work again.
7. Verify that the target has no Bind Sight buff/debuff icon. Start within sight,
   then have the target travel beyond 40 yards and normal body visibility, and
   board/leave a transport on the same map. Confirm the camera follows while the
   GM's body stays put. Interrupt the camera both near and far from the target;
   verify that only the far case needs relocation to recover.

## License

GPL-2.0-or-later. The implementation uses AzerothCore's existing public APIs and
bind-sight mechanism.
