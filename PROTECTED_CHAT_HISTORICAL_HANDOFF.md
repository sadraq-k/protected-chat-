# PROTECTED-CHAT — HISTORICAL ENGINEERING HANDOFF

This document transfers the important engineering history, verified findings, decisions, roadmap evolution, stage evidence, and permanent project constraints from the previous ChatGPT engineering conversation.

Do NOT treat this as a command to restart the audit from zero.

Use this as inherited engineering evidence and reconcile it incrementally against the CURRENT LOCAL repository.

---

# 1. CURRENT LOCAL AUTHORITY

You already verified the actual local repository:

```text
/Users/sadraq/projects/cpp/protected-chat-
```

Current known local state:

* Branch: `main`
* HEAD: `c45e82cd3bae155fc22b6fdbb80265a6d8a813f1`
* Upstream: `origin/main`

Existing local changes observed before this handoff:

### Staged

* deletion of `CMakeLists.txt`

### Unstaged

* modified `server.cpp`
* modified `.vscode/launch.json`
* modified `.vscode/tasks.json`
* deleted tracked `server` binary

These current local changes must remain untouched during reconciliation.

The current local source tree is the final source of truth.

Historical findings below must be reused where safe, but current local source overrides them when they conflict.

---

# 2. IMPORTANT HISTORICAL BASELINES

Several historical repository states were reviewed.

## Earlier audited commit

```text
9a918444a66d2c670dab2c133a572a3673a0e1a6
```

A detailed engineering audit was performed against this source state.

## Later reviewed GitHub baseline

```text
3be1c7fbf8b97e77e838c0f003dda8f17791a8c8
```

At that time, production source blobs such as:

* `server.cpp`
* `user.cpp`
* `database.cpp`
* `database.h`

were unchanged from the earlier audited production state.

The later GitHub commits had mainly introduced:

* runtime log samples;
* `spdlog-1.x/`;
* root `spdlog/`;
* merge history.

They had NOT introduced the server-side implementation changes the user originally thought had been pushed.

This is historical context only.

The current LOCAL HEAD is now different and must be reconciled only where necessary.

---

# 3. PROJECT PURPOSE

This is a university-scale C++17 client/server chat application.

Historical architecture:

```text
Client
→ synchronous Boost.Asio TCP
→ newline-delimited JSON
→ thread-per-client Server
→ small Database class
→ SQLite
```

Core features:

* registration;
* login;
* Private messaging;
* Group messaging;
* Broadcast messaging;
* offline message storage/delivery.

The project should remain understandable and relatively small.

---

# 4. ARCHITECTURAL DECISIONS TO PRESERVE

The previous engineering review concluded that the overall architecture was appropriate.

Preserve unless current local evidence gives a strong reason otherwise:

* C++17;
* synchronous Boost.Asio;
* thread-per-client;
* newline-delimited JSON;
* SQLite;
* OpenSSL/libcrypto where still needed;
* nlohmann/json;
* a small database layer;
* Private, Group, Broadcast.

Do NOT introduce without a demonstrated need:

* asynchronous Boost.Asio rewrite;
* ORM;
* connection pooling;
* repository/service layers;
* enterprise infrastructure;
* protocol replacement;
* large inheritance hierarchy;
* large framework architecture.

The project should solve local correctness, security, ownership, and concurrency problems without becoming overengineered.

---

# 5. BUILD DECISION — COMPILER-FIRST, NOT CMAKE-FIRST

A major later decision explicitly superseded the earlier CMake recommendation.

The project should NOT require CMake.

The target workflow is:

```text
clone/open repo
→ install required compiled dependencies
→ run direct compiler command
→ obtain server/client executables
```

Eventually provide direct commands for:

* GCC / `g++`
* Clang / `clang++`
* MSVC / `cl`
* MinGW where appropriate

CMake must not be the source of truth for building.

Do not replace CMake with another complicated build framework.

Historical root `CMakeLists.txt` was incomplete/invalid and was later treated as a cleanup candidate rather than something that must be repaired.

Important current-local caveat:

`CMakeLists.txt` is already staged for deletion in the current local repository.

Do NOT assume that staged deletion came from an approved historical stage.

Inspect and classify it, but do not change its staged state during reconciliation.

---

# 6. SPDLOG DECISION

`spdlog` was intentionally added.

It was NOT considered useless.

Its intended purpose is observability while debugging and testing:

* server startup/shutdown;
* authentication;
* admission;
* disconnect/reconnect;
* routing;
* socket failures;
* SQLite failures;
* concurrency behavior;
* framing;
* offline message delivery;
* regression tests.

Historical repository state contained two layouts:

```text
spdlog/
spdlog-1.x/
```

The historical conclusion was:

* do NOT delete both;
* retain ONE canonical `spdlog` dependency;
* prefer a simple header-only layout compatible with direct compiler commands;
* preserve the upstream license;
* do not make spdlog depend on CMake.

Historical analysis found root `spdlog/` duplicated the header tree under:

```text
spdlog-1.x/include/spdlog/
```

This should be rechecked only if current local dependency layout changed.

---

# 7. RUNTIME LOG FILES ARE NOT SOURCE

Distinguish:

```text
spdlog dependency
→ source/dependency
```

from:

```text
server.log
network.log
database.log
→ runtime-generated diagnostic output
```

Runtime logs should normally remain local and Git-ignored.

Logging functionality should remain.

Generated log files should not be treated as source authority.

---

# 8. SENSITIVE LOGGING DECISION

A historical audit found that authentication JSON could be logged directly.

That could expose plaintext passwords.

Permanent rule:

Never log:

* plaintext passwords;
* raw auth JSON containing passwords;
* authentication secrets;
* credentials at debug/trace level.

Prefer sanitized events such as:

```text
Authentication attempt: username=sadra
Authentication succeeded: username=sadra
Authentication failed: username=sadra, reason=invalid_credentials
```

Whether the current local source still has this defect must be revalidated if the relevant logging/auth code changed.

---

# 9. CODE SIMPLICITY IS A HARD REQUIREMENT

Permanent engineering preference:

```text
simple
clear
small
single-purpose
testable
easy to explain
easy to debug
easy to change later
```

Avoid:

* clever abstractions;
* unnecessary templates;
* excessive wrappers;
* framework-heavy design;
* unnecessary genericity;
* deep inheritance;
* God classes;
* hidden ownership;
* very large functions.

Prefer straightforward C++17 and explicit control flow.

---

# 10. SINGLE RESPONSIBILITY

Functions should normally perform one clear job.

Avoid one function that simultaneously performs unrelated responsibilities such as:

```text
parse JSON
+ authenticate
+ registry mutation
+ socket I/O
+ database access
+ cleanup
```

If a function becomes difficult to describe with one responsibility, consider splitting it.

But do not mechanically create many meaningless tiny functions.

Extraction must improve:

* readability;
* ownership;
* testability;
* responsibility separation;
* or real reuse.

---

# 11. PROJECT-OWNED STRUCT IS FORBIDDEN

Permanent project rule:

Do NOT introduce project-owned `struct`.

If a project-owned state/object unit is justified, use `class`.

Example:

```cpp
class ClientSession {
public:
    // Public interface.

private:
    // Internal state.
};
```

Do not use:

```cpp
struct ClientSession {
    ...
};
```

This rule applies only to project-owned code.

Do not modify third-party code because Boost, spdlog, nlohmann/json, or other libraries use structs internally.

If an existing project-owned struct is encountered, do not mass-refactor it automatically.

Only convert it if a future approved stage already touches that logical unit and conversion is justified.

Every future implementation review must explicitly state:

```text
New project-owned struct introduced: No
```

---

# 12. CLASS CREATION RULE

The class-only rule does NOT mean every concept should become a class.

Use:

```text
no persistent state/ownership
→ simple function

real state + behavior ownership unit
→ small class
```

A future `ClientSession` class was considered justified if one session owns:

* socket;
* authenticated username;
* persistent input buffer;
* per-socket write mutex.

It should NOT also own unrelated responsibilities such as:

* database access;
* global routing policy;
* server startup;
* logging configuration.

---

# 13. COMMENTS

All NEW project-code comments must be English.

Comments should explain only non-obvious:

* ownership;
* invariants;
* locking rules;
* lifecycle;
* framing assumptions;
* edge cases.

Do not comment obvious lines.

Do not use comments to compensate for confusing code.

If too many comments are required to explain one function, simplify the function.

---

# 14. FULL BEFORE / AFTER REVIEW RULE

Before implementation approval, the user wants COMPLETE visibility of every changed logical code unit.

A diff alone is not sufficient.

## If a function changes

Show:

```text
BEFORE — complete current function
```

and the ENTIRE current function.

Then:

```text
AFTER — complete proposed function
```

and the ENTIRE proposed function.

Do not abbreviate unchanged portions with `...`.

## If a caller changes

Show the complete changed caller BEFORE and AFTER.

## If a class changes

Show the complete class definition BEFORE and AFTER.

Show changed method implementations too.

## If a new class is introduced

Show:

* complete class;
* relevant methods;
* construction location;
* owner;
* lifetime;
* storage;
* callers;
* why the class is justified.

## If a new file is created

Show the complete proposed file before implementation.

## If a small config file changes

Show:

1. complete BEFORE;
2. complete AFTER;
3. exact diff.

The user must be able to understand the final code without mentally reconstructing it from a patch.

---

# 15. CHANGE EXPLANATION RULE

For every meaningful proposed code change explain:

## What changed?

Exact logic/state/interface difference.

## Why?

Concrete failure or limitation.

Do not merely say:

> safer

Explain the real failure scenario.

## How does the new code work?

Explain important control flow.

## Why this solution?

Explain why it is the smallest reasonable solution.

## Long-term interaction

Explain how it fits with:

* callers;
* database;
* networking;
* mutexes;
* session lifecycle;
* offline messaging;
* later roadmap stages.

Avoid short-lived code that later stages must immediately undo.

---

# 16. ALGORITHM EXPLANATION RULE

When relevant, explain important algorithms as a short sequence.

Example SQL flow:

```text
1. prepare statement
2. bind input
3. execute
4. inspect result
5. finalize
6. return outcome
```

Example session write flow:

```text
1. briefly lock registry
2. capture ClientSession
3. release registry
4. lock session write mutex
5. send one complete frame
6. return success/failure
```

Do not explain every trivial line.

Balanced depth is preferred.

---

# 17. COMPLEXITY REVIEW REQUIRED

Every future implementation proposal must explicitly answer:

1. Did any function become too large?
2. Did any function gain unrelated responsibilities?
3. Should anything be extracted?
4. Is a new class justified?
5. Is any class becoming too large?
6. Does this create unnecessary coupling?
7. Will a later stage need to undo this design?
8. Is this the simplest reasonable solution?
9. Does this introduce any new project-owned struct?

Answer to #9 must be:

```text
No
```

If the design becomes too complicated, split or simplify before implementation.

---

# 18. STAGES MUST BE SMALL

The roadmap is not sacred.

If one stage combines too many independent concerns:

STOP and split it.

Prefer:

```text
small objective
→ focused patch
→ focused tests
→ review
→ approval
```

Sub-stages such as:

```text
15A
15B
15C
```

are encouraged when they improve reasoning and testability.

---

# 19. REQUIRED 12-PART REVIEW PACKAGE

Before implementing EVERY stage, provide:

1. Stage objective
2. Complete relevant current code
3. Current behavior
4. Concrete problem / reason
5. Complete proposed code
6. Exact change explanation
7. Algorithm / control flow where relevant
8. Interaction with the rest of the project
9. Complexity review
10. Exact files/functions affected
11. Exact tests
12. Risks and approval boundary

Then STOP.

Wait for explicit implementation approval.

---

# 20. APPROVAL BOUNDARIES

Implementation approval authorizes ONLY the exact reviewed patch.

It does NOT authorize:

* staging;
* commit;
* push;
* branch;
* PR;
* merge;
* extra deletion;
* unrelated cleanup.

If implementation requires a new change that was not reviewed:

STOP.

Show complete BEFORE/AFTER first.

Get approval.

After approved implementation:

1. show exact final diff;
2. show final changed code where useful;
3. explain deviations;
4. show tests;
5. show exact Git status.

Then STOP again.

Git/GitHub actions require separate approval.

---

# 21. LOCAL EXECUTION RULE

Implementation work should happen directly in the actual local repository opened in VS Code:

```text
/Users/sadraq/projects/cpp/protected-chat-
```

Do not silently use:

* temporary clone;
* `/workspace/scratch`;
* cloud scratch checkout;
* disposable repository copy

for actual implementation.

This was a problem in the previous workflow: Stage 2A was implemented and tested in a temporary workspace, which did NOT change the user's actual local computer repository or GitHub.

That historical Stage 2A result is useful evidence, but it does not prove the current LOCAL tree contains those changes.

---

# 22. HISTORICAL DATABASE FINDINGS

These findings were previously verified against the historical audited source.

Reuse them only where the relevant current local code is unchanged.

## `insertUser()`

Historical source had already been fixed to use a prepared INSERT.

Verified historical behavior:

* fresh registration: PASS
* duplicate username: PASS
* `O'Brien`: PASS
* SQL-looking registration fields treated as data: PASS
* forced DB failure returned failure
* prepared statement finalized correctly

The prepared `insertUser()` fix existed BEFORE the later roadmap work.

Therefore, do not rewrite it automatically.

Revalidate only if current local `database.cpp` changed.

---

# 23. HISTORICAL DATABASE SCHEMA CONFLICT

Historical current-source schema used:

```text
users(..., pwd)
```

The tracked historical `chat.db` used:

```text
users(..., password)
```

Because source used:

```text
CREATE TABLE IF NOT EXISTS
```

opening the tracked database did not migrate `password` to `pwd`.

Historical tests reproduced:

* fresh source-created DB: works
* tracked `chat.db`: registration/login fails because `pwd` does not exist

Historical tracked DB was empty:

* zero users
* zero messages

This was one reason Stage 2A proposed removing tracked runtime `chat.db`.

Current local presence/schema of `chat.db` must be checked rather than assumed.

---

# 24. HISTORICAL SQL INJECTION FINDINGS

These were not theoretical. They were reproduced against the historical source.

## Login

`verifyLogin()` used raw SQL concatenation.

Severity:

```text
CRITICAL
```

Authentication bypass was reproduced.

## Offline INSERT

Offline message INSERT concatenated user-controlled sender/receiver/content and used `sqlite3_exec`.

Severity:

```text
CRITICAL
```

A multi-statement payload was reproduced that executed:

```text
DROP TABLE users
```

## Offline SELECT

Receiver input was concatenated into SELECT.

Severity:

```text
HIGH
```

Cross-user offline message retrieval was reproduced.

## Offline DELETE

Receiver input was concatenated into DELETE.

Severity:

```text
HIGH
```

Cross-user deletion was reproduced.

These historical findings should NOT be retested from zero if current local database functions are byte-equivalent/unchanged.

If those functions changed, revalidate only the affected path.

---

# 25. HISTORICAL DB ERROR REPORTING FINDING

Historically `insertUser()` exposed only a Boolean outcome.

The server mapped any failure to a duplicate-user style message such as:

```text
Username exists
```

Therefore operational SQLite failures could be misreported as username duplication.

This belonged after SQL parameterization in a database-security integration stage.

---

# 26. HISTORICAL SQLITE CONCURRENCY FINDING

Historical server had:

* one global `Database` object;
* one underlying `sqlite3*`;
* multiple detached client threads;
* no explicit application-level Database mutex.

Historical recommendation:

* one clear serialization policy;
* Database owns one non-recursive mutex;
* each public method locks once;
* avoid nested acquisition;
* never hold DB lock during socket I/O.

This remained a later stage because SQL injection had higher immediate severity.

---

# 27. HISTORICAL NETWORK FRAMING FINDING

An important correction was made during the previous audit.

## Fragmented frames

A single `boost::asio::read_until(..., '\n')` call accumulates fragmented TCP data until the delimiter.

Therefore fragmentation by itself was NOT the active bug.

## Coalesced frames

Server/client historically created a NEW local `streambuf` for each logical read.

`read_until()` may receive:

```text
frame1\nframe2\n
```

in one transport read.

The code consumed frame1 but destroyed the temporary buffer after the call, losing surplus bytes containing frame2.

Therefore the real framing issue was:

* coalesced surplus frame loss;
* no persistent per-connection receive buffer.

A later framing stage should preserve newline-delimited JSON but introduce persistent buffering.

---

# 28. HISTORICAL FRAME LIMIT FINDING

Historical `read_until()` flow had no explicit maximum frame size.

A peer could send a large stream without newline and cause memory growth.

Severity:

```text
HIGH
```

Historical solution direction:

* keep newline framing;
* add a modest maximum frame size;
* reject oversized frames predictably.

---

# 29. HISTORICAL SOCKET WRITE CONCURRENCY FINDING

Historical server allowed multiple threads to write to the same socket without a per-socket write mutex.

Even though `boost::asio::write` handles partial writes within ONE call, separate concurrent write calls may interleave at the application-message level.

Historical recommendation:

* one write mutex per connected session;
* lock around the complete serialized JSON + newline frame.

---

# 30. HISTORICAL REGISTRY LOCKING FINDING

Historical global clients mutex protected an online-user map.

It was also held during:

* blocking socket writes;
* some database operations.

Consequence:

A slow client could stall unrelated:

* routing;
* admission;
* cleanup;
* database fallback.

Historical target rule:

```text
registry lock
→ only map access
→ capture shared session
→ release
→ perform network/DB work outside global registry lock
```

---

# 31. HISTORICAL SESSION OWNERSHIP DIRECTION

Historical registry stored roughly:

```text
username → shared socket
```

A future lightweight session ownership object was considered justified.

Because project-owned structs are now forbidden, the future type must be:

```cpp
class ClientSession
```

not a struct.

Potential owned state:

* socket;
* authenticated username;
* persistent receive buffer;
* write mutex.

The purpose is ownership correctness, not architecture expansion.

---

# 32. HISTORICAL DISCONNECT / RECONNECT BUG

Historical cleanup used username-based removal equivalent to:

```text
clients.erase(username)
```

Failure scenario:

1. old session disconnects;
2. same user reconnects quickly;
3. replacement session enters registry;
4. old handler cleanup runs late;
5. username-only erase removes the NEW session.

Severity:

```text
HIGH
```

Historical target:

Remove only if the registry still points to the exact session being cleaned up.

---

# 33. HISTORICAL DUPLICATE LOGIN / ADMISSION BUG

Historically authentication SUCCESS could be sent before registry admission was complete.

Possible result:

```text
SUCCESS
then
FAIL
```

for duplicate login.

Historical target:

```text
authenticate
→ attempt admission
→ determine final outcome
→ send one final response
```

---

# 34. HISTORICAL SEND FAILURE FINDING

Historical send helper returned `void` and caught/logged exceptions.

Callers could not distinguish:

* successful delivery;
* failed delivery.

This became especially dangerous for offline message draining.

Historical target:

write operation returns real success/failure.

---

# 35. HISTORICAL OFFLINE MESSAGE FLOW

Historical flow:

```text
fetch every queued receiver row
→ attempt every send
→ clear every receiver row
```

Historical problems:

## Message loss

If a send failed mid-drain, receiver-wide clear could still delete unsent messages.

## Concurrent insertion loss

A new message inserted AFTER fetch but BEFORE receiver-wide delete could be deleted even though it had never been fetched or sent.

Historical target:

```text
fetch stable message IDs
→ send each message
→ delete only IDs successfully sent
```

Exactly-once delivery was considered unnecessary.

Accepted project-scale guarantee:

```text
at-least-once-style
```

with possible crash-window duplicates.

Do NOT add complex acknowledgement infrastructure unless future requirements justify it.

---

# 36. HISTORICAL PRIVATE/GROUP/BROADCAST FINDINGS

## Private

Historically online receiver lookup delivered directly.

Offline receiver fallback stored a message.

Receiver existence was not validated against registered users.

Therefore typo/nonexistent names could generate orphan offline rows.

## Group

Historically iterated supplied receivers.

Duplicate receiver entries could cause duplicate delivery/storage.

There was no normalization/deduplication.

## Broadcast

Historically iterated only the currently online client map.

Therefore offline Broadcast did NOT exist.

A later stage was intentionally designed to require a user decision:

* Broadcast online-only?
* or queue for registered offline users?

Do not invent semantics without user approval.

---

# 37. HISTORICAL UNKNOWN MESSAGE TYPE BUG

Historical routing used an `else` branch as Broadcast.

Therefore an unknown type could accidentally enter Broadcast behavior.

Severity:

```text
HIGH
```

Historical target:

explicit message-type allowlist.

Invalid type must be rejected, never silently rerouted.

---

# 38. HISTORICAL JSON VALIDATION FINDING

Historical server directly indexed/converts JSON fields without systematic validation of:

* required fields;
* types;
* lengths;
* group receiver element types.

Malformed input could throw and disconnect clients or enter incorrect paths.

A later protocol-validation stage was planned.

---

# 39. HISTORICAL PASSWORD STORAGE FINDING

Historical password storage used one unsalted SHA-256 digest.

Severity:

```text
MEDIUM
```

Historical later-stage direction:

* salted password KDF;
* PBKDF2-HMAC-SHA256 was considered appropriate using existing OpenSSL dependency;
* versioned/stored metadata if needed;
* compatibility/migration decision explicitly designed first.

This was intentionally delayed until SQL/auth/locking behavior was stable.

---

# 40. HISTORICAL CLIENT LIFECYCLE FINDING

Historical client used:

* main/input thread;
* receive thread.

Shutdown state/socket behavior were not centrally owned.

Possible issues:

* receive thread blocked while main closes socket;
* server disconnect occurs while main remains blocked on console input;
* shutdown/join races.

Historical roadmap split this into:

* client lifecycle design;
* implementation.

---

# 41. HISTORICAL REPOSITORY HYGIENE FINDINGS

Historical GitHub tracked items included some combination of:

* `chat.db`
* root `server`
* root `client`
* `.vscode/...`
* runtime logs
* generated `proj_in_1file.md`
* snapshot generator `proj_in_1file (1).py`
* duplicate `spdlog` layouts
* incomplete `CMakeLists.txt`

Important distinction:

`proj_in_1file (1).py` was later identified as a useful generator and should NOT automatically be considered obsolete.

Its default output was historically:

```text
proj_in_1file.md
```

The output was generated/stale and was considered a cleanup candidate.

---

# 42. HISTORICAL STAGE 2A REVIEW

Stage 2A was carefully reduced to ONE responsibility:

```text
Generated and Runtime Artifact Cleanup
```

It deliberately excluded:

* C++ changes;
* spdlog canonicalization;
* CMake decision;
* README work;
* generator removal.

Historical final Stage 2A reviewed deletion list was:

```text
chat.db
server
client
.vscode/launch.json
.vscode/settings.json
.vscode/tasks.json
logs/database.log
logs/network.log
logs/server.log
proj_in_1file.md
```

Historical `.gitignore` additions were:

```gitignore
/server
/client
*.db-wal
*.db-shm
/proj_in_1file.md
```

Existing ignore rules already covered:

```text
.vscode/
*.db
*.log
```

Historical review also checked whether:

```text
.vscode/c_cpp_properties.json
```

still existed.

Final historical GitHub review found it ABSENT at that baseline, so it was not added to the deletion list.

---

# 43. HISTORICAL STAGE 2A TEMP-WORKSPACE RESULT

Stage 2A was implemented and tested in a temporary ChatGPT/agent workspace.

Reported result:

```text
PASS
```

Reported historical effects:

* `.gitignore` changed exactly as reviewed;
* exactly ten approved files removed;
* zero C++ changes;
* zero spdlog changes;
* fresh DB used `pwd`;
* registration regressions passed;
* no staging;
* no commit;
* no push.

Important:

THIS DID NOT MODIFY THE USER'S ACTUAL LOCAL COMPUTER REPOSITORY.

It also did NOT modify GitHub because no commit/push happened.

Therefore this result is:

```text
historical implementation/test evidence
```

NOT:

```text
proof of current local Stage 2A state
```

Use this evidence to avoid repeating conceptual analysis, but inspect current local status to determine which Stage 2A changes are already present manually.

---

# 44. HISTORICAL STAGE 2B DECISION

Stage 2B was separated from Stage 2A:

```text
Canonical Header-Only spdlog Layout
```

Historical intent:

* retain root `spdlog/` as one canonical header-only dependency;
* preserve upstream license;
* remove redundant full `spdlog-1.x/` only after separate review/approval;
* do NOT delete both dependency trees.

No Stage 2B implementation was historically approved or completed.

Revalidate current local dependency tree before assuming this is still needed.

---

# 45. HISTORICAL STAGE 3 DECISION

The earlier CMake repair stage was superseded.

Stage 3 became:

```text
Cross-Platform Compiler-First Build
```

Objective:

Document/test direct C++17 compile/link commands for:

* Linux GCC;
* macOS Clang;
* Windows MSVC;
* MinGW where appropriate.

No required CMake.

Platform commands that cannot actually be executed in the current environment must be clearly marked:

```text
unverified
```

Do not pretend cross-platform execution evidence exists when it does not.

---

# 46. LOGGING ROADMAP EVOLUTION

The old single logging stage was considered too broad.

It was split:

## 10A — Logging Design

Design only.

Decide:

* logger responsibilities;
* sinks;
* levels;
* credential-redaction invariant;
* useful event inventory.

## 10B — Minimal spdlog Foundation

Small focused logging foundation.

No logging framework hierarchy.

## 10C — Authentication Redaction

Remove password/raw auth JSON exposure.

## 10D — Network Diagnostic Events

Connection/read/write/framing errors.

## 10E — Application and Session Events

Startup, auth outcome, admission, routing, cleanup.

## 10F — Database Diagnostic Events

SQLite result/error evidence.

This split was intentional so later concurrency/network/offline stages have good observability.

---

# 47. HISTORICAL CORRECTED ROADMAP

The latest historical roadmap before moving to local Codex was:

## Stage 0 — Baseline / Freeze

Historically complete.

## Stage 1 — Audit `insertUser()`

Historically complete.

## Stage 2A — Generated and Runtime Artifact Cleanup

## Stage 2B — Canonical Header-Only `spdlog` Layout

## Stage 3 — Cross-Platform Compiler-First Build

## Stage 4 — Revalidate Existing `insertUser()` Fix

## Stage 5 — Parameterize `verifyLogin()`

## Stage 6 — Parameterize Offline INSERT

## Stage 7 — Parameterize Offline SELECT

## Stage 8 — Parameterize Offline DELETE

## Stage 9 — Database Security Integration

## Stage 10A — Logging Design

## Stage 10B — Minimal `spdlog` Foundation

## Stage 10C — Authentication Redaction

## Stage 10D — Network Diagnostic Events

## Stage 10E — Application and Session Events

## Stage 10F — Database Diagnostic Events

## Stage 11 — SQLite Concurrency Design

## Stage 12 — Implement DB Serialization

## Stage 13A — Server Persistent Framing and Limit

## Stage 13B — Client Persistent Framing and Limit

## Stage 14 — Server Session and Lock Design

## Stage 15A — Introduce `ClientSession` CLASS

Never project-owned struct.

## Stage 15B — Serialize Socket Writes and Return Outcomes

## Stage 15C — Shorten Registry Critical Sections

## Stage 16A — Atomic Admission and Duplicate Login

## Stage 16B — Identity-Safe Disconnect Cleanup

## Stage 16C — Handler Lifetime and Failure Cleanup

## Stage 17 — Offline Delivery Design

## Stage 18A — ID-Based Offline Database APIs

## Stage 18B — Success-Aware Offline Drain

## Stage 19A — Recipient Validation and Group Deduplication

## Stage 19B — Broadcast Semantics

## Stage 20A — Client Lifecycle Design

## Stage 20B — Implement Client Lifecycle

## Stage 21 — JSON Protocol Validation

## Stage 22A — Password KDF and Schema Design

## Stage 22B — Password Storage Hardening

## Stage 23 — Final Integration and Compiler-First Documentation

This roadmap is historical, not immutable.

Use current local code to determine whether:

* some stages are already partly implemented;
* some local changes belong to later stages;
* stage order should be adjusted;
* a stage should be split further.

Do NOT merge stages merely to reduce stage count.

---

# 48. HISTORICAL DEPENDENCY ORDER

The engineering dependency logic was approximately:

```text
repository/source-authority cleanup
→ canonical spdlog
→ compiler-first build
→ accept existing insertUser fix
→ SQL security
→ safe structured logging
→ DB serialization
→ persistent framing
→ session ownership/write locking
→ admission/disconnect correctness
→ safe offline delivery
→ routing semantics
→ client lifecycle
→ protocol validation
→ password KDF
→ final integration/docs
```

Reasoning:

* SQL injection is critical and locally repairable early.
* Logging should become safe/useful before difficult concurrency/network stages.
* Offline delivery depends on reliable send outcomes and session ownership.
* Protocol/password work should not destabilize earlier core correctness stages.

---

# 49. TESTING PHILOSOPHY

Every implementation stage should test:

* normal success;
* expected failure;
* relevant edge cases;
* preservation of existing behavior.

Compilation alone is not enough.

A stage is complete only when:

```text
code is understandable
+ responsibilities remain clear
+ relevant tests pass
+ existing behavior is preserved
+ unnecessary complexity was not introduced
```

Later stages should rerun relevant previous regressions.

---

# 50. HISTORICAL FINAL TEST COVERAGE EXPECTATIONS

Eventually cover:

* direct clean compiler builds where available;
* registration;
* duplicate registration;
* apostrophes;
* SQL-looking registration data;
* correct/wrong login;
* login injection attempts;
* Private;
* Group;
* Broadcast;
* offline delivery;
* interrupted offline drain;
* concurrent insertion during drain;
* two senders to one receiver;
* slow receiver;
* fragmented frames;
* coalesced frames;
* oversized frame;
* malformed JSON;
* unknown message type;
* duplicate login;
* rapid reconnect;
* EXIT;
* abrupt client disconnect;
* abrupt server disconnect;
* multi-client regression;
* log inspection proving no password exposure.

---

# 51. IMPORTANT: DO NOT REPEAT WORK UNNECESSARILY

Use this policy for historical evidence.

## If relevant current local code is unchanged

Preserve the historical finding.

Do not re-audit or rerun expensive tests merely to rediscover it unless needed for the next stage's Definition of Done.

Mark it as:

```text
Inherited verified evidence
```

and state the baseline it came from.

## If relevant local code changed

Revalidate ONLY the affected finding.

Example:

If `server.cpp` changed but `database.cpp` did not:

* revalidate server/session/routing/logging findings affected by `server.cpp`;
* do not automatically retest historical SQL injection in unchanged database methods.

## If evidence came from temporary workspace

Preserve it as historical evidence.

Do not claim it proves current LOCAL file state.

## If current source contradicts history

Current source wins.

Record the contradiction.

---

# 52. CURRENT LOCAL DELTA DESERVES SPECIAL ATTENTION

You already observed:

* local HEAD differs from the historical GitHub baseline;
* `server.cpp` is locally modified;
* `CMakeLists.txt` is staged for deletion;
* `.vscode/launch.json` and `.vscode/tasks.json` are locally modified;
* root `server` executable is locally deleted.

Therefore, current reconciliation should focus especially on:

## `server.cpp`

Determine what changed and whether it:

* implements logging;
* introduces or modifies spdlog;
* changes routing;
* changes admission;
* changes disconnect behavior;
* changes registry locking;
* changes session ownership;
* changes offline delivery;
* introduces new bugs;
* partially completes historical roadmap stages.

Do not overwrite it.

## `CMakeLists.txt`

Classify the staged deletion.

Because compiler-first/no-required-CMake is a valid project decision, deletion MAY be consistent with project direction.

But it was not historically authorized as part of Stage 2A.

Do not infer approval.

## `.vscode` local modifications

Treat them as current local state.

Determine whether they are:

* developer-local convenience;
* compiler-first configuration work;
* stale IDE setup;
* unrelated experiments.

Do not overwrite them.

## deleted `server` binary

Likely consistent with historical Stage 2A artifact cleanup, but classify based on current local evidence.

---

# 53. PERSISTENT PROJECT KNOWLEDGE DESIGN

After incremental reconciliation, prepare THREE proposed repository-local files.

Do NOT create them yet.

---

## A. `AGENTS.md`

Purpose:

Stable Codex/project engineering rules.

It should contain primarily:

* local repository execution rule;
* source-of-truth rule;
* simplicity requirement;
* single responsibility;
* class-only project-owned OO types;
* no project-owned struct;
* class justification rule;
* English comments;
* compiler-first/no-required-CMake rule;
* spdlog intentionality;
* password logging prohibition;
* small-stage requirement;
* full BEFORE/AFTER visibility;
* 12-part review package;
* complexity review;
* implementation approval boundary;
* Git approval boundary;
* test-after-each-stage rule.

Do NOT overload `AGENTS.md` with long historical bug descriptions.

---

## B. `PROJECT_CONTEXT.md`

Purpose:

Current project state.

It should contain:

* current local HEAD;
* current branch;
* current dirty/staged status;
* current architecture;
* current dependencies;
* current source organization;
* current protocol;
* current database schema;
* current threading/networking model;
* CURRENT active issues;
* CURRENT resolved issues;
* current roadmap;
* current stage progress;
* exact immediate next stage;
* current decisions awaiting user approval.

This file will evolve.

---

## C. `ENGINEERING_HISTORY.md`

Purpose:

Why the project reached its current state.

It should preserve:

* important historical commit SHAs;
* major audits;
* verified historical findings;
* SQL injection evidence;
* framing correction;
* concurrency findings;
* offline delivery reasoning;
* architecture preservation decision;
* compiler-first decision;
* spdlog decision;
* no-struct/class-only decision;
* stage splitting decisions;
* Stage 2A review and temporary-workspace PASS evidence;
* superseded CMake recommendation;
* superseded overly broad cleanup assumptions;
* important roadmap evolution.

It should NOT contain chat filler or repetitive conversational text.

---

# 54. WHAT TO OMIT FROM PERSISTENT HISTORY

Do NOT copy raw chat transcript.

Omit:

* greetings;
* repeated confirmations;
* duplicated versions of the same prompt;
* superseded wording when a later decision replaced it;
* irrelevant conversational filler;
* obsolete roadmap copies that add no reasoning value.

Preserve the DECISION and WHY.

---

# 55. CURRENT TASK AFTER RECEIVING THIS HANDOFF

Do NOT implement project code.

Do NOT modify the repository yet.

Perform incremental reconciliation only.

Return:

# 1. Handoff Acceptance

Briefly state that the historical engineering context has been ingested.

# 2. Historical Evidence Reuse Map

List which important findings can be reused without re-analysis because their relevant current code is unchanged.

# 3. Required Local Revalidation Map

List only findings that require revalidation because relevant local code changed.

# 4. Current Local Change Classification

Classify each existing staged/unstaged local change.

# 5. Roadmap Reconciliation

Show current status of historical stages:

* COMPLETE
* PARTIAL
* IMPLEMENTED BUT UNVERIFIED
* NOT STARTED
* SUPERSEDED
* REQUIRES USER DECISION

# 6. Recommended Immediate Next Stage

Based on current local evidence.

Do NOT implement it.

# 7. Complete Proposed `AGENTS.md`

Show the entire file.

# 8. Complete Proposed `PROJECT_CONTEXT.md`

Show the entire file.

# 9. Complete Proposed `ENGINEERING_HISTORY.md`

Show the entire file.

# 10. Revalidation/Test Economy

Explain what historical analysis/tests you intentionally did NOT repeat and why.

Then STOP.

Wait for my explicit approval before creating any of the three files.

Do not:

* edit existing source;
* reset anything;
* stage;
* unstage;
* commit;
* push;
* branch;
* PR;
* merge;
* implement the next stage.

