# Skill proposals

Improvements to skill language/process discovered by the skill-test loop
(`skill-test-loop.md`). We do not edit skill prompts directly from the loop — each entry
here is a documented, evidence-backed proposal for a later deliberate edit. Each entry
names the skill, the associated test (if any), the evidence, and the proposed change.

(Note: this file previously held a misplaced copy of the bug fix-loop doc; replaced
2026-07-02 when the skill-test loop first ran.)

## Proposals

(populated by the loop — one section per finding)

### /position — ThirdPerson mesh offset is BP-template-authored, not an ACharacter C++ default (TASK-1)

- **Skill**: `.claude/skills/position/SKILL.md`, §"The default ThirdPerson mesh offset (a UE
  template convention)".
- **Test**: `tests/skills/test_position_conventions.py` (the coordinate-convention battery;
  3/3 green against a live editor 2026-07-02).
- **Evidence** (UE 5.7 source, read this task): the `ACharacter` constructor sets
  `CapsuleComponent->InitCapsuleSize(34.0f, 88.0f)`
  (`Engine/Source/Runtime/Engine/Private/Character.cpp:77`), but the Mesh subobject block
  (`Character.cpp:118-133`) only creates the component, attaches it to the capsule, and sets
  collision/tick flags — it assigns **no** `RelativeLocation`/`RelativeRotation`. The
  `(0,0,-90)` / `Yaw -90` offset the skill describes exists only where a Blueprint template
  (e.g. ThirdPerson `BP_ThirdPersonCharacter`) authors it; `CacheInitialMeshOffset`
  (`Character.cpp:149,191-194`) merely caches whatever the template authored. Confirmed live:
  a bare spawned `/Script/Engine.Character` reads back capsule 34/88, with no mesh offset in
  play (test 2 of the battery).
- **Proposed change**: the skill already labels the offset "a UE template convention", which
  is correct — sharpen it one notch so an agent never expects the offset on a bare
  `ACharacter`: e.g. "This offset is authored in the ThirdPerson **Blueprint template**, NOT
  in `ACharacter`'s C++ constructor (`Character.cpp:118-133` sets no relative offset) — a
  bare spawned Character has mesh offset (0,0,0)/yaw 0."
- **Bonus fixture note for future test authors** (not a skill defect): the engine test mesh
  `/Engine/EngineMeshes/SkeletalCube` has its root bone authored pitched ~+90° (bone-local +X
  points at component +Z), so a root-bone `forward_world` at actor rotation zero is ~(0,0,1),
  not (1,0,0). TASK-1's spec assumed identity authoring; the battery cancels the authored
  pitch with actor pitch −90 before testing yaw handedness. Any future test that treats a
  SkeletalCube bone's forward as the actor forward must do the same.
