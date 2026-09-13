---
name: ctf-triage
description: First-five-minutes triage for a capture-the-flag challenge.
---

Run this before touching the challenge proper. It exists to stop the classic
failure: an hour of exploitation against a misread category.

1. **Category and points** -- what the challenge says it is. Reversing
   rated easy is a different exercise from pwn rated hard; let the rating
   set the expected difficulty of the intended path.
2. **Artifacts** -- `file *` on everything. What each artifact actually is
   (ELF? pcap? ciphertext with a suspicious header?) outranks what it is
   named.
3. **Protections** -- `checksec` on any binary. NX off changes the playbook;
   so does a canary or PIE.
4. **Strings** -- `strings -n 8` and look for flag formats, library
   versions, embedded URLs. Cheap, and embarrassingly often decisive.
5. **Provided material** -- Dockerfiles, source, handouts. If source ships,
   the challenge is source-review first, binary second.

Write down the one-line conclusion of each step. If triage produces a
hypothesis ("stack overflow, no PIE, canary absent -- look for the read into
a fixed buffer"), pursue it; if it produces nothing, the absence is the
finding -- the challenge is hiding its surface deliberately.
