# Protected Chat — Current Project Context

## Reconciliation snapshot

Date: 2026-09-06.

Working source:
`/Users/sadraq/projects/cpp/protected-chat-`

Branch: `main`
HEAD: `c45e82cd3bae155fc22b6fdbb80265a6d8a813f1`
Upstream tracking ref:
`origin/main` at `3be1c7fbf8b97e77e838c0f003dda8f17791a8c8`

No fetch was performed. Remote freshness is not established.

This snapshot records the reconciled local state and the subsequently
inspected local/upstream divergence. It is not a claim that the tree is clean
or that any roadmap implementation is approved.

Observed Git status:

```text
## main...origin/main [behind 3]
 M .vscode/launch.json
 M .vscode/tasks.json
D  CMakeLists.txt
 D server
 M server.cpp
?? AGENTS.md
?? ENGINEERING_HISTORY.md
?? PROJECT_CONTEXT.md
?? PROTECTED_CHAT_HISTORICAL_HANDOFF.md
```

Existing changes remain user-owned and untouched.

## Evidence boundaries

The historical audited baseline was:
`9a918444a66d2c670dab2c133a572a3673a0e1a6`

The later historical reviewed baseline was:
`3be1c7fbf8b97e77e838c0f003dda8f17791a8c8`

Both historical commit objects are now available through the locally recorded
origin/main history.

Read-only divergence inspection established:

- Local main is the merge base and is three commits behind origin/main.
- 9a918444a66d2c670dab2c133a572a3673a0e1a6 modifies only database.cpp
  and contains the historically verified prepared insertUser() fix.
- 4e29dca28b94227314e391e67e681c45eaf3e6ed adds three runtime logs,
  a complete spdlog-1.x tree, and a duplicate header tree under spdlog/.
- 3be1c7fbf8b97e77e838c0f003dda8f17791a8c8 merges those histories
  without an additional conflict-resolution patch.

No upstream commit has been adopted or approved for automatic adoption.
Current local source remains authoritative.

database.cpp, database.h, user.cpp, and .gitignore have no working-tree
changes relative to local HEAD. Direct comparison now confirms that local
database.cpp lacks the prepared insertUser() implementation in 9a918444.

Inherited findings are retained where the described defective code patterns
remain. Historical test results are not presented as current local test runs.

No compiler, application, exploit, concurrency, or lifecycle tests were run.
chat.db was inspected using a read-only immutable SQLite connection.

## Purpose and architecture

A university-scale terminal chat application supporting registration, login,
Private, Group, Broadcast, and offline messages.

Preserved target architecture:

Client → synchronous Boost.Asio TCP → newline-delimited JSON
→ thread-per-client server → small Database class → SQLite.

C++17 is the engineering target. Current README examples still specify
C++11; they do not implement the agreed compiler-first C++17 workflow.

The server accepts connections synchronously and detaches one handler thread
per connection. A global mutex protects a username-to-shared-socket map.
One global Database object owns one SQLite connection.

The client uses a console/input thread and a receiving thread.

Both source entry points use 127.0.0.1:1403.

## Source and artifact organization

| Path | Role |
|---|---|
| server.cpp | Startup, authentication, admission, routing, sends, receive parsing, offline drain, cleanup |
| user.cpp | Actual client source, authentication UI, send loop, receive thread |
| database.h | Database interface, SHA-256 helper, existing Message struct |
| database.cpp | SQLite schema initialization and user/offline operations |
| README.md | Existing documentation with known implementation/build discrepancies |
| .gitignore | Partial artifact ignore policy |
| .vscode/launch.json | Tracked debugger configuration, locally modified |
| .vscode/tasks.json | Tracked compiler tasks, locally modified |
| .vscode/settings.json | Tracked editor settings |
| chat.db | Tracked runtime database with incompatible users schema |
| client | Tracked client executable artifact |
| server | Tracked executable, absent from working tree |
| CMakeLists.txt | Absent; deletion already staged |
| proj_in_1file (1).py | Useful snapshot generator; preserve |
| proj_in_1file.md | Tracked generated snapshot; not source authority |
| PROTECTED_CHAT_HISTORICAL_HANDOFF.md | User-provided inherited context, currently untracked |

The generator defaults to proj_in_1file.md.

No root spdlog/, spdlog-1.x/, logs/, or
.vscode/c_cpp_properties.json exists in the observed tree.

## Dependencies and build state

Source uses:

- Boost.Asio.
- nlohmann/json.
- SQLite3.
- OpenSSL SHA-256 functionality.
- Standard C++ threading and synchronization.

spdlog remains an intentional future dependency decision, but neither
historical vendored tree exists locally and current source does not integrate
it. System-installed dependency availability was not checked.

Current logging uses standard output/error.

Compiler-first builds must target direct C++17 commands for GCC, Clang,
MSVC, and MinGW where appropriate. CMake is not required.

README names client.cpp, but the client source is user.cpp.
The added Homebrew g++-16 task compiles only the active file and does not
supply explicit C++17, project translation units, or dependency/link flags.
Its paired LLDB launch entry is not proof of a working build/debug workflow.

All platform build outcomes remain unverified for this local snapshot.

## Protocol

Transport: TCP with one JSON object followed by newline.

Authentication actions:

- SIGN_IN: name, username, password.
- LOG_IN: username, password.

Responses use status and message.

Client messaging:

- PRIVATE: receiver and content.
- GROUP: receivers array and content.
- BROADCAST: content.
- EXIT.

Delivered messages use type MESSAGE, sender, and content.

Current routing explicitly handles EXIT, PRIVATE, and GROUP, then treats
other types as Broadcast. An explicit allowed-type check is not implemented.

Current buffer lifetime can discard coalesced surplus frames.
There is no explicit maximum frame size.

Broadcast iterates the client registry, not all registered database users.
It may queue for stale entries encountered in that map, but does not provide
general offline Broadcast delivery.

## Database schema and registration conflict

Source-created schema:

```sql
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    username TEXT NOT NULL UNIQUE,
    pwd TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender TEXT NOT NULL,
    receiver TEXT NOT NULL,
    message TEXT NOT NULL
);
```

Read-only inspection of tracked chat.db found:

- users has password instead of pwd.
- messages has id, sender, receiver, message.
- users row count: 0.
- messages row count: 0.

CREATE TABLE IF NOT EXISTS does not migrate the existing users table.

Local insertUser() hashes the password, concatenates an INSERT string, and
passes it through executeQuery() to sqlite3_exec.

This contradicts the inherited prepared-INSERT implementation and its PASS
results. The historical fix must not be marked present or accepted locally.
The origin of this discrepancy is unknown.

## Current local changes

- server.cpp: only the address parser changed from address::from_string(ip)
  to boost::asio::ip::make_address(ip). This is a narrow API adjustment;
  build/startup are unverified. It changes no historical routing, admission,
  logging, locking, session, or offline logic.
- CMakeLists.txt: staged deletion of a one-line target_link_libraries call.
  Consistent with compiler-first direction, but excluded from historical
  Stage 2A. Approval/provenance is not inferred.
- .vscode/tasks.json: adds a macOS Homebrew active-file compiler task.
- .vscode/launch.json: adds the corresponding LLDB launch configuration.
  These are developer-local conveniences, not a completed build stage.
- server: working-tree deletion aligns with artifact cleanup, but the binary
  remains in the index.
- The historical handoff is newly present and untracked.

## Active issues

These are inherited findings whose described implementation patterns remain,
except where explicitly identified as a current contradiction.
Historical reproductions are documented in ENGINEERING_HISTORY.md.

### Database and credentials

- Registration uses concatenated SQL: historical prepared fix absent.
  Focused parameterization and registration regression evidence are needed.
- Login SQL concatenation: inherited critical authentication-bypass finding.
- Offline INSERT concatenation through sqlite3_exec: inherited critical
  multi-statement injection finding.
- Offline SELECT/DELETE concatenation: inherited high-severity cross-user
  access/deletion findings.
- Tracked database schema conflicts with the source.
- Registration maps operational database failures to Username exists.
- No explicit application-level Database serialization policy.
- Password storage uses unsalted SHA-256.
- Server and client can print raw authentication JSON containing passwords.

### Networking, sessions, and routing

- Temporary receive buffers lose surplus coalesced frames.
- No explicit maximum frame size.
- No per-session complete-frame write mutex.
- Registry critical sections include blocking network and database work.
- Authentication SUCCESS can precede failed duplicate-login admission.
- Cleanup removes by username without checking session identity.
- Send helper catches failures and returns void.
- Offline drain clears all receiver rows after send attempts, risking loss
  of unsent rows and rows inserted between fetch and clear.
- No registered-recipient validation.
- No Group recipient deduplication.
- General offline Broadcast is absent; intended semantics await user choice.
- Unknown types fall through to Broadcast.
- JSON validation is incomplete.
- Client shutdown, console blocking, socket ownership, and thread joining
  need the planned lifecycle work.

### Repository and documentation

- Runtime database, executable, editor files, and generated snapshot remain
  tracked.
- Stage 2A's five additional ignore patterns are absent.
- spdlog dependency layout differs from the historical baseline.
- README references nonexistent client.cpp, describes JSON as future work,
  and overstates offline Broadcast behavior.
- An existing project-owned Message struct remains. Do not convert it outside
  an approved stage touching that logical unit.
- Existing non-English comments do not authorize unrelated comment changes.

## Resolved issues and verified progress

No behavioral bug fix is newly verified by this reconciliation.

The local working location, branch, HEAD, dirty state, database schema, and
dependency inventory are established.

Historical corrections remain accepted:

- Fragmentation alone was not the framing defect.
- CMake repair is not the build direction.
- The snapshot generator is useful.
- spdlog is intentional.
- Temporary Stage 2A PASS does not prove local completion.

The one-line server address change is implemented but unverified.
Its presence does not complete a roadmap stage.

## Roadmap and stage progress

Statuses refer to current local delivery unless explicitly marked historical.
Inherited design direction does not imply completed design-stage review.

| Stage | Objective | Status and qualification |
|---|---|---|
| 0 | Baseline / Freeze | COMPLETE historically; local read-only baseline captured, existing dirty state preserved |
| 1 | Audit insertUser | COMPLETE historically; prepared-fix conclusion conflicts with local source |
| 2A | Generated/runtime artifact cleanup | PARTIAL: server absent; historical logs absent; remaining tracked artifacts and ignore additions unresolved |
| 2B, historical | Canonicalize existing spdlog trees | SUPERSEDED locally: neither tree exists; does not block Stage 3 or SQL-security work |
| 3 | Cross-platform compiler-first build | PARTIAL: existing commands/editor configuration; no complete verified C++17 workflow |
| 4 | Revalidate existing prepared insertUser fix | SUPERSEDED locally: assumed fix absent |
| 4A, proposed | Parameterize local insertUser and verify registration | REQUIRES USER DECISION: replacement scope, no patch approved |
| 5 | Parameterize verifyLogin | NOT STARTED |
| 6 | Parameterize offline INSERT | NOT STARTED |
| 7 | Parameterize offline SELECT | NOT STARTED |
| 8 | Parameterize offline DELETE | NOT STARTED |
| 9 | Database security integration | NOT STARTED |
| 10A | Logging design | NOT STARTED; inherited requirements retained |
| 10A.1, proposed | Establish one canonical header-only spdlog dependency | NOT STARTED; small separately reviewed stage after 10A, immediately before 10B; preserve upstream license and direct-compiler compatibility |
| 10B | Minimal spdlog foundation | NOT STARTED; follows dependency establishment |
| 10C | Authentication redaction | NOT STARTED |
| 10D | Network diagnostic events | NOT STARTED |
| 10E | Application/session events | NOT STARTED |
| 10F | Database diagnostic events | NOT STARTED |
| 11 | SQLite concurrency design | NOT STARTED; inherited direction retained |
| 12 | Implement DB serialization | NOT STARTED |
| 13A | Server persistent framing and limit | NOT STARTED |
| 13B | Client persistent framing and limit | NOT STARTED |
| 14 | Server session and lock design | NOT STARTED; inherited direction retained |
| 15A | Introduce ClientSession class | NOT STARTED |
| 15B | Serialize writes and return outcomes | NOT STARTED |
| 15C | Shorten registry critical sections | NOT STARTED |
| 16A | Atomic admission / duplicate login | NOT STARTED |
| 16B | Identity-safe cleanup | NOT STARTED |
| 16C | Handler lifetime / failure cleanup | NOT STARTED |
| 17 | Offline delivery design | NOT STARTED; inherited direction retained |
| 18A | ID-based offline database APIs | NOT STARTED |
| 18B | Success-aware offline drain | NOT STARTED |
| 19A | Recipient validation / Group deduplication | NOT STARTED |
| 19B | Broadcast semantics | REQUIRES USER DECISION |
| 20A | Client lifecycle design | NOT STARTED |
| 20B | Implement client lifecycle | NOT STARTED |
| 21 | JSON protocol validation | NOT STARTED |
| 22A | Password KDF / schema design | NOT STARTED; migration decision required |
| 22B | Password storage hardening | NOT STARTED |
| 23 | Final integration / compiler-first docs | NOT STARTED |

### Approved local stage order

The current local absence of both historical vendored spdlog trees changes
the dependency order. Historical Stage 2B canonicalization is not a
prerequisite for compiler-first builds or SQL-security work.

The intended order is:

Stage 2A — reconcile artifact cleanup
→ Stage 3 — cross-platform compiler-first build
→ Stage 4A — parameterize current local insertUser and verify registration
→ Stages 5–9 — SQL security and database-security integration
→ Stage 10A — logging design
→ Stage 10A.1 — establish one canonical header-only spdlog dependency
→ Stage 10B onward — structured logging
→ continue the remaining roadmap.

Stage 10A.1 is a small, separately reviewed dependency stage immediately
before the logging foundation that needs it. Do not introduce spdlog earlier
merely to preserve historical numbering.

This order is user-approved planning direction, not implementation approval.

No entire current local stage is classified IMPLEMENTED BUT UNVERIFIED.
That description applies only to the narrow server address API edit.

## Immediate next stage and pending decisions

Current authorized work is reconciliation and presentation of complete
knowledge-file proposals. No repository write is authorized by that task.

After knowledge-file approval, recommended next implementation proposal:

Stage 2A — Generated and Runtime Artifact Cleanup, reconciled to local state.

Prepare a full review using the actual remaining inventory:

- chat.db.
- client.
- .vscode/launch.json.
- .vscode/settings.json.
- .vscode/tasks.json.
- proj_in_1file.md.
- Already absent but indexed server.
- Missing ignore additions: /server, /client, *.db-wal, *.db-shm,
  /proj_in_1file.md.

Historical logs are absent and need no deletion.
Preserve the generator.
Keep CMake and spdlog decisions outside Stage 2A.
Do not discard modified editor configurations based on historical approval.
Determine and review how their current contents will be preserved or handled.
Index changes require separate authorization.

Decisions awaiting the user:

1. Exact current Stage 2A implementation patch.
2. Whether any upstream commit should ever be adopted; no adoption is implied
   by the divergence inspection.
3. Exact spdlog acquisition/layout patch at Stage 10A.1, after logging design
   and immediately before Stage 10B; it does not block build or SQL stages.
4. Exact Stage 4A implementation using the inspected prepared registration
   fix as prior evidence.
5. Any further adjustment to the approved local stage order.
6. Broadcast online-only versus queuing for registered offline users.
7. Password KDF metadata and compatibility/migration policy at Stage 22A.
8. Separate authorization for any Git mutation or handling of existing staged
   CMake deletion.

## Verification economy

Reconciliation used Git metadata/diffs, relevant source already inspected,
targeted source checks, file presence checks, generator default inspection,
and read-only database schema/count queries.

No full audit, historical exploit replay, registration suite, concurrency
suite, runtime startup, build, dependency installation, or network fetch was
performed.

Future tests should target approved changes and their relevant regressions.
Historical prepared-registration PASS cannot substitute for tests of the
different local implementation.
