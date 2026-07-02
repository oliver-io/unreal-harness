# Skills

We have a host of skills in the repository that purport to be useful in various circumstances.  Some of them are untestable, like architecture, but some of them are testable, like networking and third-party service usage (neo4j).

The task here is to examine each skill and its associated tests (`tests/skills`) or the associated tests with the guidance it is giving (i.e., the skill may be using some specific already-tested feature, in which case it is well-tested).

Analyze if we can test it.  Analyze what is tested and if there are gaps.  Double check that we are not over-engineering or trying to test something that is inherently difficult.

Once we have decided that the skill lacks some domain of testing, we should document it in `./docs/loops/skills/TASKS.md`, documenting the intended task, and move on to the next skill.

**IMPORTANT**: All good tests are strongly verifiable.  If we do not believe we can verify the outcome of a successful usage, do not add it to the list.

**GOAL**: to create solid tests for the future, and proposals in `./docs/loops/skills/PROPOSALS.md` for improved language or process in the skills.  We do not directly alter the prompts, but rather, document the improvements we believe are required.

# Fix Loop Orchestrator

Act as an orchestrator of subagents once we have assembled our task list:
We should pull an item from the list, and task a subagent with the following our procedure, with our exterior loop just orchestrating and keeping track of agent progress.  Each subagent should:

## Fix Loop Task

Take a single item off of the list.  Understand its report.  If we cannot understand the bug report, we should move it to a #DEFERRED list in the bug document, and clearly state that it was not understood or does not seem accurate at the time.  We should not take any bug reports at face value, instead, always:

1) understand the lack of testing, and in theory, how we could test it
2) investigate the MCP and the associated Unreal Engine plugin code if necessary to understand what we are testing
3) correlate our MCP and C++ plugin code with **actual source engine code** for the Unreal Engine if we are testing deeply integrated features
 a) if we cannot find the source engine code, we should **ABORT** this process and stop the loop, and explain to the user.
4) set up a test case using the test harness, matching the intended case
5) set up the way we will verify success
6) run the test case; be careful not to be overly-eager to fix implementations, as mostly they are working.  it is likely the skill that is misguided.
7) iterate on the test case until we have a good test for the processes; if we find any obvious pitfalls in the skill process, document it in **`./docs/loops/skills/PROPOSALS.md`**

## One Item At a Time

We address one test case at a time.  We take it as an isolated report, research, set it up it, and iterate until we believe we have verified the given process rigorously or encountered a failure mode in the skill (for documentation of PROPOSALS) it if possible.

## Failure Cases or Stuck Modes

If we are failing to act or stuck in any way, we should simply **ABORT** and terminate this loop process, explaining the difficulty to the user.

## Per Item Finished

Document your changes with a git commit to *ONLY* **TESTS** and **PROPOSALS.md** and the intent of the proposal, and the test case it was associated with.  Then, move on to the next.

This should never touch an implementation.  If we find that there is an implementation issue causing the failure, we should document it separately in **BUGS.md** according to the CLAUDE.md of this project.

# Loop

We should continue looping until there are no more skills to test that we believe have verifiable sane test cases.

# Completion

When complete, double check our work, delete any looping chronjob, and summarize all the changes.