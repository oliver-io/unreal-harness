# Documentation Updates

Documentation-update tasks are tracked in [TASKS](./TASKS.md). This document should be kept as a living record of all the current documentation tasks, and when something is updated, if it doesn't need explicit confirmation, should be removed from the task list. We want to keep this list as a pristine pending-work list, and not confuse anyone with completed tasks. Another process is responsible for queueing the actual update tasks onto the list — this loop only consumes them.

The tasks may span any part of the harness's documentation surface: tool descriptions and their `ToolDef` guidance, architecture docs (`docs/ARCHITECTURE.md`), code self-documentation (comments, headers, `CLAUDE.md` files), guides and skills (`.claude/skills/`), usage contracts (`docs/USAGE.md`), READMEs, and changelogs.

If it does not already have one, we should make sure that the task document has a #DEFERRED list in case we don't know how to handle it.

# Update Loop Orchestrator

We should pull an item from the task list, and task a subagent with the following entire procedure, with our exterior loop just orchestrating and keeping track of agent progress. Each subagent should:

## Update Loop Task

Take a single task off of the list. Understand its request. If we cannot understand the task, we should move it to a #DEFERRED list in the task document, and clearly state that it was not understood or does not seem accurate at the time. We should not take any task descriptions at face value, instead, always:

1) understand the task, and form a picture of what documentation is wrong, missing, or stale
2) investigate the current documentation and the associated MCP server / Unreal Engine plugin code
3) correlate the documentation's claims with the **actual code** — the server tool registry, the C++ handlers, the scripts, the skills; the code is ground truth, never the docs
 a) if we cannot find the ground-truth code for a documented claim, we should **ABORT** this process and stop the loop, and explain to the user.
4) once we know the correct current state, double check our work and make sure the task makes sense
5) update the documentation, and if necessary, any associated cross-references (indexes, changelogs, sibling docs that repeat the claim)

## One Item At a Time

We address one task at a time. We take it as an isolated request, research it, verify the true state of the code, and update the docs if possible. If we cannot verify the true state or complete the update, we move it to the #DEFERRED list.

## Failure Cases or Stuck Modes

If we are failing to act or stuck in any way, we should simply **ABORT** and terminate this loop process, explaining the difficulty to the user.

## Per Item Finished

Document your changes with a git commit specific to the altered files and the intent of the changes. Then, move on to the next.

# Loop

We should continue looping until there are no more tasks in our task list, or we have encountered some kind of a hard-stop issue.

# Completion

When complete, double check our work, delete any looping chronjob, and summarize all the changes.
