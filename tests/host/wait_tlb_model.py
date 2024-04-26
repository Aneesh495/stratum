#!/usr/bin/env python3
"""Explore bounded wait enrollment and translation ownership independently of the kernel."""

from collections import deque
from dataclasses import dataclass, replace


@dataclass(frozen=True)
class Wait:
    phase: str = "new"
    changed: bool = False
    notified: bool = False
    mapped: bool = True
    elapsed: bool = False
    completions: int = 0


def wait_steps(state, split_check=False):
    def finish():
        return replace(state, phase="done", completions=state.completions + 1)

    if state.phase == "new":
        if not state.mapped or state.changed or state.elapsed:
            yield "reject wait", finish()
        elif split_check:
            yield "check before enrollment", replace(state, phase="checked")
        else:
            yield "check and enroll", replace(state, phase="parked")
    if state.phase == "checked":
        yield "enroll without checking again", replace(state, phase="parked")
    if not state.changed:
        yield "publish changed word", replace(state, changed=True)
    if state.changed and not state.notified:
        after = finish() if state.phase == "parked" else state
        yield "notify waiter", replace(after, notified=True)
    if not state.elapsed:
        after = finish() if state.phase == "parked" else state
        yield "deadline expires", replace(after, elapsed=True)
    if state.mapped:
        after = finish() if state.phase == "parked" else state
        yield "unmap and cancel", replace(after, mapped=False)


def wait_invariant(state):
    return state.completions <= 1 and not (
        state.phase == "parked" and (state.notified or state.elapsed or not state.mapped)
    )


CPUS = 3
ALL = (1 << CPUS) - 1


@dataclass(frozen=True)
class Translation:
    published: bool = False
    active: int = ALL
    cached_old: int = 0
    acknowledged: int = 0
    reused: bool = False


def tlb_steps(state, accept_stale=False):
    if not state.published:
        yield "replace mapping and publish generation", replace(state, published=True)
    for cpu in range(CPUS):
        mask = 1 << cpu
        if state.active & mask:
            yield f"CPU {cpu} leaves and flushes CR3", replace(
                state, active=state.active & ~mask, cached_old=state.cached_old & ~mask
            )
            if not state.published and not state.cached_old & mask:
                yield f"CPU {cpu} caches old translation", replace(state, cached_old=state.cached_old | mask)
        else:
            yield f"CPU {cpu} enters with flushed CR3", replace(state, active=state.active | mask)
        if state.published and not state.acknowledged & mask:
            yield f"CPU {cpu} invalidates and acknowledges current generation", replace(
                state, cached_old=state.cached_old & ~mask, acknowledged=state.acknowledged | mask
            )
            if accept_stale:
                yield f"accept old acknowledgment from CPU {cpu}", replace(
                    state, acknowledged=state.acknowledged | mask
                )
    if state.acknowledged == ALL and not state.reused:
        yield "reuse retired frame", replace(state, reused=True)


def tlb_invariant(state):
    return not (state.reused and state.cached_old)


def explore(initial, transitions, invariant):
    seen = {initial}
    pending = deque([(initial, ())])
    edges = 0
    while pending:
        state, history = pending.popleft()
        if not invariant(state):
            return len(seen), edges, history
        for event, successor in transitions(state):
            edges += 1
            if successor not in seen:
                seen.add(successor)
                pending.append((successor, (*history, event)))
    return len(seen), edges, None


def verify(name, initial, transitions, broken_transitions, invariant):
    states, edges, failure = explore(initial, transitions, invariant)
    if failure:
        raise RuntimeError(f"{name}: unexpected counterexample {failure}")
    _, _, witness = explore(initial, broken_transitions, invariant)
    if not witness:
        raise RuntimeError(f"{name}: negative control escaped detection")
    print(f"[PASS] {name} explored {states} states and {edges} transitions")
    print(f"[PASS] {name} rejected negative control: {' -> '.join(witness)}")


if __name__ == "__main__":
    verify("wait", Wait(), wait_steps, lambda state: wait_steps(state, split_check=True), wait_invariant)
    verify("TLB", Translation(), tlb_steps, lambda state: tlb_steps(state, accept_stale=True), tlb_invariant)
