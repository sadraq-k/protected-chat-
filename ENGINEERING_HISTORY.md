# Protected Chat — Engineering History

## Provenance and authority

This document preserves engineering findings and decisions inherited from
PROTECTED_CHAT_HISTORICAL_HANDOFF.md and records the local reconciliation
performed on 2026-09-06.

It is a decision and evidence record, not a raw conversation transcript.

Historical audited commit:
`9a918444a66d2c670dab2c133a572a3673a0e1a6`

Later historical reviewed GitHub baseline:
`3be1c7fbf8b97e77e838c0f003dda8f17791a8c8`

Current local reconciliation HEAD:
`c45e82cd3bae155fc22b6fdbb80265a6d8a813f1`

The handoff reports that server.cpp, user.cpp, database.cpp, and database.h
were unchanged between the two historical baselines. Later historical GitHub
changes mainly added runtime logs, two spdlog layouts, and merge history;
they did not contain the server implementation changes previously expected.

During the initial local reconciliation, the historical commit objects were
unavailable. They later became available through the locally recorded
origin/main history and were inspected without adopting them.

Direct comparison established that:

- 9a918444a66d2c670dab2c133a572a3673a0e1a6 contains the historical
  prepared insertUser() implementation and modifies only database.cpp.
- 4e29dca28b94227314e391e67e681c45eaf3e6ed adds the historical runtime
  logs and both spdlog trees.
- 3be1c7fbf8b97e77e838c0f003dda8f17791a8c8 merges those histories.
- Current local main is the parent of 9a918444 and therefore does not contain
  its prepared insertUser() correction.

This inspection strengthens the historical provenance but does not authorize
pulling, merging, cherry-picking, or otherwise adopting the upstream commits.

Use these evidence categories:

- Inherited verified evidence: historical findings/results reported by the
  handoff against the historical audited source.
- Historical temporary-workspace evidence: results from a separate workspace.
- Current local static evidence: source, Git, and artifact observations.
- Current local runtime evidence: only tests actually executed locally.

Preserve useful historical evidence without representing it as a current
local test result. Current source wins on conflict.

## Architecture preservation

The previous review accepted a university-scale C++17 architecture using
synchronous Boost.Asio, one thread per client, newline-delimited JSON,
SQLite, nlohmann/json, and OpenSSL/libcrypto where needed.

The goal is to repair correctness, security, ownership, and concurrency while
keeping the program small and understandable.

Async rewrites, ORMs, connection pools, enterprise layers, deep inheritance,
and protocol replacement were rejected absent demonstrated need.

## Build decision evolution

An earlier CMake repair recommendation was superseded.

The accepted direction became compiler-first:

clone/open → install dependencies → direct compiler command → executables.

Target compiler families are GCC, Clang, MSVC, and MinGW where appropriate.
CMake must not be required or become build authority.
Unexecuted platform commands must be labelled unverified.

The historical root CMakeLists.txt was incomplete and became a cleanup
candidate, not something that necessarily needed repair.

Its deletion was explicitly outside Stage 2A.
Current local staged deletion does not establish historical approval.

## spdlog and logging decisions

spdlog was intentionally introduced for startup/shutdown, authentication,
admission, disconnect/reconnect, routing, socket and SQLite failures, framing,
offline delivery, concurrency diagnosis, and regression testing.

Historical analysis found root spdlog/ duplicated the header tree beneath
spdlog-1.x/include/spdlog/.

The decision was to retain one canonical, preferably header-only dependency,
preserve the upstream license, and keep direct compiler compatibility.
Deleting both dependencies was not the agreed cleanup.

Runtime server.log, network.log, and database.log are generated diagnostics,
not source authority. They should normally be local and ignored.

Historical raw authentication JSON logging could expose plaintext passwords.
The permanent invariant prohibits password, secret, and credential logging
at every level. Sanitized authentication events were preferred.

A single broad logging stage was split into:

- 10A: design of responsibilities, sinks, levels, redaction, and events.
- 10B: minimal spdlog foundation.
- 10C: authentication redaction.
- 10D: network diagnostics.
- 10E: application/session events.
- 10F: database diagnostics.

The split keeps each patch reviewable and establishes useful observability
before difficult networking/concurrency stages.

## Code and review decisions

The project requires small, clear, single-purpose code and explicit ownership.

Project-owned struct declarations are forbidden for new code.
Use class for justified state/behavior ownership; use functions otherwise.
Do not alter third-party structs or automatically refactor existing ones.

A small ClientSession class was considered justified to own a socket,
authenticated identity, persistent receive buffer, and write mutex.
It should not absorb database access, routing policy, startup, or logging
configuration.

All new project-code comments must be English and explain non-obvious facts.

Implementation requires complete BEFORE/AFTER visibility for changed logical
units and complete proposed new files. Diff-only review is insufficient.
Small configuration changes require full BEFORE, full AFTER, and exact diff.

Every stage requires the twelve-part review package and explicit complexity
review recorded in AGENTS.md.

Implementation approval covers only the exact reviewed patch.
Git actions, extra deletion, and unrelated cleanup require separate approval.
New unreviewed changes require another review before application.

## Historical registration evidence

Inherited verified evidence — historical audited source, reported by handoff:

insertUser() used a prepared INSERT before the later roadmap work.

Reported results:

- Fresh registration: PASS.
- Duplicate username handling: PASS.
- O'Brien: PASS.
- SQL-looking registration fields treated as data: PASS.
- Forced database failure returned failure.
- Prepared statement finalized correctly.

This justified auditing and accepting the existing fix instead of rewriting it.

Current reconciliation contradicts that implementation claim:
local insertUser() concatenates SQL and calls sqlite3_exec through
executeQuery(). Historical PASS remains valid as a report about the historical
implementation, not as evidence of current registration safety.

The discrepancy's origin is unknown. Do not label it a proven regression
between commits without source evidence.

## Historical schema conflict

Inherited verified evidence:

Source expected users.pwd.
Tracked chat.db contained users.password.
CREATE TABLE IF NOT EXISTS did not migrate the existing table.

Historical tests reported:

- Fresh source-created database worked.
- Tracked database registration/login failed because pwd was missing.
- Tracked database contained zero users and zero messages.

This supported treating chat.db as a runtime artifact cleanup candidate.

Current read-only inspection independently confirmed the same schema mismatch
and zero row counts. Application tests were not repeated.

## Historical SQL injection evidence

Inherited verified evidence — historical audited source:

| Path | Historical severity | Reproduced result |
|---|---|---|
| verifyLogin | CRITICAL | Authentication bypass |
| Offline INSERT | CRITICAL | Multi-statement payload executed DROP TABLE users |
| Offline SELECT | HIGH | Cross-user offline message retrieval |
| Offline DELETE | HIGH | Cross-user offline message deletion |

The relevant methods concatenated untrusted values into SQL.
Using sqlite3_prepare_v2 on already-concatenated SQL did not provide
parameter binding.

These historical exploit tests were not rerun during local reconciliation.
The corresponding concatenation patterns remain in local source.

Registration's missing prepared fix is an additional local contradiction;
the historical registration PASS cannot be transferred to it.

## Database error and concurrency findings

Registration exposed only a Boolean result.
The server mapped failure to a duplicate-user message, masking operational
SQLite errors. Better error integration was scheduled after parameterization.

One global Database object and underlying sqlite3 connection were shared
across detached client threads without an explicit application-level
Database mutex.

The proposed serialization direction was:

- Database owns one non-recursive mutex.
- Each public method locks once.
- Avoid nested acquisition.
- Do not hold the database lock during socket I/O.

This was a later stage because the SQL injection findings had higher immediate
priority and could be repaired locally.

## Framing correction and size limit

An earlier framing interpretation was corrected.

A single read_until call accumulates fragmented TCP data until the newline.
Fragmentation by itself was not the identified bug.

The real issue was temporary buffer lifetime:
read_until could receive frame1 and frame2 together, consume frame1, and lose
frame2 when the local streambuf was destroyed.

The target is persistent per-connection buffering while preserving
newline-delimited JSON.

A separate high-severity issue was the absence of an explicit frame-size
limit, permitting memory growth from input without a delimiter.
A modest bound and predictable rejection were planned.

Server and client framing work were split into stages 13A and 13B.

## Socket writes, registry locks, and sessions

Historical server writes lacked a per-socket write mutex.
boost::asio::write handling partial writes within one call does not serialize
different concurrent callers.

The target is serialization of each complete JSON-plus-newline frame.

The global registry lock also covered blocking writes and some database work,
allowing one slow client to stall unrelated routing, admission, and cleanup.

The intended sequence is:

1. Briefly lock the registry.
2. Capture the shared session.
3. Release the registry lock.
4. Perform network or database work outside that lock.
5. Serialize writes with the session's write mutex.
6. Return a real send outcome.

Session work was split into introducing ClientSession, serializing writes
with outcomes, and shortening registry critical sections.

## Admission and cleanup findings

Authentication SUCCESS could be sent before admission completed, producing
SUCCESS followed by FAIL for duplicate login.

The target is authenticate → attempt admission → determine final result
→ send one final response.

Cleanup erased the username without verifying session identity.
Historical reasoning identified the risk of late old-session cleanup removing
a replacement session.

The target is to remove a registry entry only if it still identifies the exact
session being cleaned up.

Admission, identity-safe cleanup, and handler lifetime/failure cleanup were
split into stages 16A, 16B, and 16C.

## Offline delivery reasoning

The historical send helper returned void and caught exceptions, so callers
could not distinguish successful and failed delivery.

Offline flow fetched all receiver rows, attempted sends, then cleared all
receiver rows.

Two loss cases motivated redesign:

- Failed sends could still be followed by deletion of unsent messages.
- Messages inserted after fetch but before receiver-wide deletion could be
  deleted without ever being fetched or sent.

The target is stable message IDs, real send outcomes, and deletion only of IDs
successfully sent.

The accepted project-scale direction is at-least-once-style delivery with
possible crash-window duplicates. It is a target, not a current guarantee.
Exactly-once delivery and complex acknowledgement infrastructure were not
required.

Work was split into design, ID-based database APIs, and success-aware drain.

## Routing, validation, passwords, and client lifecycle

Historical findings retained:

- Private messages did not validate registered receiver existence, allowing
  orphan offline rows for nonexistent names.
- Group did not deduplicate receiver lists.
- Broadcast iterated only the client registry, so general offline Broadcast
  did not exist.
- Unknown message types entered the default Broadcast branch.
- JSON required fields, types, lengths, and receiver elements lacked
  systematic validation.
- Passwords used one unsalted SHA-256 digest.
- Client input and receive threads lacked sufficiently clear coordinated
  shutdown/socket ownership, with console blocking and join concerns.

Broadcast semantics deliberately require a user choice:
online-only or queue for registered offline users.

Password hardening was delayed until core SQL/auth/concurrency behavior was
stable. PBKDF2-HMAC-SHA256 using OpenSSL was considered appropriate, with
metadata and migration designed before implementation.

Client lifecycle and password work each received separate design and
implementation stages.

## Repository hygiene and corrected assumptions

Historical tracked artifacts included runtime databases, executables,
editor files, runtime logs, generated snapshots, duplicate dependency trees,
and incomplete CMake configuration.

The snapshot generator proj_in_1file (1).py was later recognized as useful.
Its default output proj_in_1file.md was generated and potentially stale.

The corrected cleanup distinction was:

- Preserve useful generator source.
- Treat generated snapshots and runtime artifacts separately.
- Keep spdlog intentional.
- Review duplicate dependency removal separately.
- Keep the build decision separate.

## Stage 2A review and temporary-workspace evidence

Stage 2A was narrowed to Generated and Runtime Artifact Cleanup.

It excluded C++ changes, spdlog canonicalization, CMake work, README work,
and generator removal.

The historical approved deletion list contained ten files:

1. chat.db
2. server
3. client
4. .vscode/launch.json
5. .vscode/settings.json
6. .vscode/tasks.json
7. logs/database.log
8. logs/network.log
9. logs/server.log
10. proj_in_1file.md

Historical ignore additions:

- /server
- /client
- *.db-wal
- *.db-shm
- /proj_in_1file.md

Existing rules already covered .vscode/, *.db, and *.log.
.vscode/c_cpp_properties.json was absent at final historical review and was
not included in the deletion list.

Historical temporary-workspace evidence reported PASS:

- Exact reviewed ignore changes.
- Exactly ten approved removals.
- Zero C++ changes.
- Zero spdlog changes.
- Fresh database used pwd.
- Registration regressions passed.
- No staging, commit, or push.

This did not modify the user's actual local repository or GitHub.
It must not be used to claim local Stage 2A completion or authorize replay
over subsequently modified local files.

Stage 2B, canonical header-only spdlog, was separate and was not historically
implemented or approved for implementation.

## Roadmap evolution and dependency order

Historically completed work was Stage 0 baseline/freeze and Stage 1
insertUser audit.

Major corrections:

- Broad cleanup split into artifact cleanup and dependency canonicalization.
- CMake repair superseded by compiler-first build.
- Broad logging split into six stages.
- Framing split by server/client.
- Session work split into ownership, writes, and registry locking.
- Admission and cleanup split into focused concerns.
- Offline work split into design, database APIs, and drain.
- Routing split recipient rules from Broadcast semantics.
- Client lifecycle and password work split design from implementation.

The intended dependency order was:

source/artifact clarity → canonical spdlog → compiler-first build
→ accept prepared registration → SQL security → safe useful logging
→ DB serialization → persistent framing → sessions and write locking
→ admission/cleanup → safe offline delivery → routing decisions
→ client lifecycle → JSON validation → password KDF → final integration.

The historical dependency order above is preserved as decision history.

After local reconciliation, the user approved a stage-order correction
because neither historical vendored spdlog tree exists locally.
Canonicalizing those absent trees must not block Stage 3 or critical
SQL-security work.

The approved local order is:

Stage 2A artifact cleanup
→ Stage 3 compiler-first build
→ Stage 4A parameterize current local insertUser and verify registration
→ Stages 5–9 SQL security and database-security integration
→ Stage 10A logging design
→ Stage 10A.1 establish one canonical header-only spdlog dependency
→ Stage 10B onward structured logging
→ continue the remaining roadmap.

Historical Stage 2B is superseded locally. Dependency establishment becomes
a small separately reviewed stage immediately before the logging foundation
that needs it. Preserve the upstream license and direct-compiler compatibility.

This preserves spdlog's intentional role while avoiding premature dependency
work solely to retain historical numbering. The order correction does not
authorize dependency installation, implementation, or Git changes.

PROJECT_CONTEXT.md contains the current stage-by-stage roadmap.

## Testing philosophy and eventual coverage

Every implementation stage must cover normal success, expected failure,
relevant edges, and preservation of existing behavior.
Compilation alone is insufficient.

Eventually cover:

- Direct clean compiler builds on available platforms.
- Registration, duplicates, apostrophes, SQL-looking data, database failure.
- Correct/wrong login and login injection attempts.
- Private, Group, Broadcast, and offline delivery.
- Interrupted offline drain and insertion during drain.
- Two senders to one receiver and slow receivers.
- Fragmented, coalesced, oversized, and malformed frames.
- Unknown message types.
- Duplicate login and rapid reconnect.
- EXIT and abrupt client/server disconnect.
- Multi-client regressions.
- Log inspection proving absence of password exposure.

Repeat prior regressions where affected changes or stage acceptance require
them. Do not rerun entire historical audits without cause.

## Local reconciliation record — 2026-09-06

Read-only reconciliation established:

- Actual local main at c45e82cd3bae155fc22b6fdbb80265a6d8a813f1.
- Existing staged CMake deletion.
- Existing unstaged server binary deletion.
- Existing modified VS Code task/launch files.
- Only one working-tree server source edit: address parsing API replacement.
- Local registration uses raw SQL rather than the historical prepared fix.
- Neither historical spdlog directory nor logs directory exists.
- Tracked database still has password rather than pwd and contains no user
  or message rows.
- Historical Stage 2A ignore additions are absent.
- The generator remains present with its historical default output.
- The three proposed knowledge files do not yet exist.

No files or Git state were changed.
No build, runtime, exploit, or concurrency suite was run.
No temporary clone, network fetch, or dependency installation was used.

The preserved findings are inherited evidence with current static
applicability, not newly reproduced local test results.

## Local/upstream divergence reconciliation — 2026-09-06

Read-only history inspection established:

- Local main: c45e82cd3bae155fc22b6fdbb80265a6d8a813f1.
- Locally recorded origin/main:
  3be1c7fbf8b97e77e838c0f003dda8f17791a8c8.
- Local main is zero commits ahead and three commits behind.
- The aggregate upstream delta is 286 files, 61,172 insertions, and
  8 deletions.
- The only project C++ delta is the prepared insertUser() change in
  database.cpp.
- The other aggregate additions are three runtime logs, 177 paths under
  spdlog-1.x/, and 105 duplicate header paths under spdlog/.
- The current dirty working-tree paths do not directly overlap the aggregate
  upstream paths.

Although mechanically non-overlapping, adopting all three commits would
conflict with the approved roadmap by adding tracked runtime logs and both
historical spdlog trees before they are needed. No upstream commit was
adopted.
