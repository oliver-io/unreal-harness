# MCP Bugs

MCP and harness bugs are tracked in [bugs](../../BUGS.md).  This document should be kept as a living record of all the current bugs, and when something is fixed, if it doesn't need explicit confirmation, should be removed from the bugs list.  We want to keep this list as a pristine known issues list, and not confuse anyone with solved problems.

If it does not already have one, we should make sure that the bug report document has a #DEFERRED list in case we don't know how to handle it.

# Fix Loop Orchestrator

We should pull an item from the bug list, and task a subagent with the following entire procedure, with our exterior loop just orchestrating and keeping track of agent progress.  Each subagent shoukd:

## Fix Loop Task

Take a single bug off of the list.  Understand its report.  If we cannot understand the bug report, we should move it to a #DEFERRED list in the bug document, and clearly state that it was not understood or does not seem accurate at the time.  We should not take any bug reports at face value, instead, always:

1) understand the bug, and form a hypothesis about what could cause it
2) investigate the MCP and the associated Unreal Engine plugin code
3) correlate our MCP and C++ plugin code with **actual source engine code** for the Unreal Engine
 a) if we cannot find the source engine code, we should **ABORT** this process and stop the loop, and explain to the user.
4) once we have an RCA, double check our work and make sure the bug report makes sense
5) fix the issue, and if necessary, any associated documentation

## One Item At a Time

We address one bug at a time.  We take it as an isolated report, research, RCA it, and fix it if possible.  If we cannot fix it or produce an RCA, we move it to the #DEFERRED list/

## Failure Cases or Stuck Modes

If we are failing to act or stuck in any way, we should simply **ABORT** and terminate this loop process, explaining the difficulty to the user.

## Per Item Finished

Document your changes with a git commit specific to the altered files and the intent of the changes.  Then, move on to the next.

# Loop

We should continue looping until there are no more bugs in our bug list, or we have encountered some kind of a hard-stop issue.

# Completion

When complete, double check our work, delete any looping chronjob, and summarize all the changes.