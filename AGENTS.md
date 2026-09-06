# Protected Chat — Engineering Rules

## Authority and working location

Work directly in the actual repository opened locally in VS Code:

`/Users/sadraq/projects/cpp/protected-chat-`

Do not silently implement in a temporary clone, cloud checkout, scratch
repository, or disposable repository copy.

Current local source is the final authority for implementation facts.
User-approved engineering decisions govern intended behavior.

Read PROJECT_CONTEXT.md for current state and ENGINEERING_HISTORY.md for
inherited findings, evidence, and decision history.

Historical notes, generated snapshots, runtime logs, and temporary-workspace
test results do not prove current local implementation state.

Preserve historical verified findings where relevant source is unchanged.
Revalidate only affected findings when source differs, or when a stage's
acceptance criteria require relevant regression tests.
Do not equate unchanged-from-HEAD with unchanged-from-a-historical-baseline.
State when historical source equivalence cannot be established.

Inspect current Git status before proposing changes. Preserve existing staged,
unstaged, and untracked work. Never reset, overwrite, discard, stage, or unstage
user changes without explicit authorization for that action.

## Architecture and simplicity

Preserve the university-scale architecture unless an approved requirement
justifies a change:

- C++17.
- Synchronous Boost.Asio TCP.
- Thread per client.
- Newline-delimited JSON using nlohmann/json.
- SQLite through a small database layer.
- OpenSSL/libcrypto where needed.
- Private, Group, and Broadcast messaging.

Prefer simple, clear, small, single-purpose, testable code with explicit
ownership and control flow.

Do not introduce asynchronous rewrites, ORMs, connection pools, repository or
service layers, deep inheritance, large frameworks, unnecessary templates,
generic wrappers, or enterprise infrastructure without demonstrated need.

Functions should have one clear responsibility. Extract code when it improves
readability, ownership, testability, responsibility separation, or real reuse.
Do not mechanically create meaningless tiny functions.

## Classes and comments

Do not introduce project-owned struct declarations.

Use a simple function when no persistent state or ownership is needed.
Use a small class only for a justified state-and-behavior ownership unit.
The class-only rule does not require turning every concept into a class.

A future ClientSession class may own its socket, authenticated identity,
persistent input buffer, and write mutex. It should not also own database
access, global routing policy, server startup, or logging configuration.

Do not alter third-party structs to satisfy this rule.
Do not mass-convert existing project-owned structs. Convert an existing unit
only when an approved stage touches it and the conversion is justified.

Every implementation review must explicitly state:

`New project-owned struct introduced: No`

All new project-code comments must be English.
Explain non-obvious ownership, invariants, locking, lifecycle, framing, or edge
cases. Do not comment obvious operations or use comments to conceal confusing
code. Existing comments do not authorize unrelated cleanup.

## Build and dependency decisions

The build workflow is compiler-first. CMake must not be required or become
the source of truth.

Target direct C++17 compiler/link commands for GCC, Clang, MSVC, and MinGW
where appropriate. Do not replace CMake with another complicated framework.

Mark platform commands unverified unless actually executed on that platform.
Compiler or IDE configuration text is not execution evidence.

spdlog is an intentional observability dependency.
Retain or establish one approved canonical, preferably header-only layout
with its upstream license and compatibility with direct compiler commands.
Inspect the actual dependency inventory before proposing canonicalization.
Do not blindly delete both historical dependency trees or assume they exist.

Runtime logs are generated diagnostic output and should normally be local
and Git-ignored. Preserve useful logging functionality.

Never log plaintext passwords, raw authentication JSON containing passwords,
authentication secrets, or credentials at any level, including debug/trace.
Use sanitized events. Existing violations require a reviewed correction;
they do not authorize an unreviewed logging rewrite.

## Small stages and complete review

Keep stages focused: one small objective, a focused patch, focused tests,
review, and approval. Split broad stages into sub-stages when needed.
The roadmap may evolve; do not merge unrelated concerns to reduce stage count.

Before implementing every stage, provide all twelve items:

1. Stage objective.
2. Complete relevant current code.
3. Current behavior.
4. Concrete problem or reason.
5. Complete proposed code.
6. Exact change explanation.
7. Algorithm or control flow where relevant.
8. Interaction with the rest of the project.
9. Complexity review.
10. Exact files and functions affected.
11. Exact tests.
12. Risks and approval boundary.

Then stop and wait for explicit implementation approval.

For every changed function, show the complete current function BEFORE and
complete proposed function AFTER. Include complete changed callers.

For changed classes, show complete class definitions BEFORE and AFTER and
the changed method implementations.

For new classes, show the complete class, relevant methods, construction
location, owner, lifetime, storage, callers, and justification.

For a new file, show its complete proposed contents before creation.
For a small configuration file, show complete BEFORE, complete AFTER, and
the exact diff.

Do not abbreviate code with ellipses or require the user to reconstruct final
code from a patch. A diff alone is insufficient.

Explain each meaningful change:

- What logic, state, or interface changes.
- The concrete failure scenario or limitation.
- How the proposed control flow works.
- Why this is the smallest reasonable solution.
- Effects on callers, database access, networking, mutexes, session lifecycle,
  offline delivery, and later stages where applicable.

Describe important algorithms as short sequences without explaining every
trivial line. Avoid temporary designs that later stages immediately undo.

## Required complexity review

Explicitly answer:

1. Did any function become too large?
2. Did any function gain unrelated responsibilities?
3. Should anything be extracted?
4. Is a new class justified?
5. Is any class becoming too large?
6. Does this create unnecessary coupling?
7. Will a later stage need to undo this design?
8. Is this the simplest reasonable solution?
9. Does this introduce any new project-owned struct?

The answer to item 9 must be No.
Simplify or split an overly complicated proposal before implementation.

## Approval boundaries

Implementation approval authorizes only the exact reviewed patch.

It does not authorize staging, unstaging, commits, pushes, branch creation,
PR creation, merges, extra deletion, or unrelated cleanup.
Git and GitHub mutations require separate explicit approval.

If implementation requires an additional unreviewed change, stop, present
complete BEFORE/AFTER contents, and obtain approval for that change.

After approved implementation:

1. Show the exact final diff.
2. Show final changed code where useful.
3. Explain any deviations.
4. Report test commands, results, and limitations.
5. Show exact Git status.

Then stop. Do not infer permission for a subsequent stage or Git action.

## Testing and completion

After each approved implementation stage, test normal success, expected
failure, relevant edge cases, and preservation of existing behavior.
Compilation alone is insufficient for behavioral changes.

Rerun relevant earlier regressions when the change or stage acceptance
criteria justify it. Do not repeat historical audits or expensive tests just
to rediscover unchanged findings.

Distinguish:

- Inherited verified evidence and its historical baseline.
- Current local static inspection.
- Current local executed tests.
- Historical temporary-workspace results.
- Unverified assumptions.

A stage is complete only when its code is understandable, responsibilities
remain clear, relevant tests pass, existing behavior is preserved, and no
unnecessary complexity was introduced.
