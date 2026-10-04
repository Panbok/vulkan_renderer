---
status: proposed
updated: 2026-10-04
authority: proposal
---
# World partition

Work that remains after the world partition
([ADR-086](../adr/086-world-partition.md)): terrain streaming, cells and cell
documents, the streaming runtime, origin rebasing during Play, baked proxies
and the partition tools are implemented. Defects and verification gaps of the
implemented work are in the
[level toolkit audit](level-toolkit-audit.md).

## Current baseline

- Terrains up to 8,192 cells stream fine tiles around the streaming sources
  and draw a coarse overview elsewhere; one windowed height field body
  follows the sources.
- Editor-created root entities stream by 128 m cells from per-cell documents;
  the primary scene alone partitions. Imported document entities, the root
  World and added scenes keep every entity.
- `vkr_bakery bake proxies` builds per-cell proxies from brushes only.
- During Play the origin rebases past 4 km and Reset restores it exactly.
- Moving over a streamed terrain with local shadows on raises the frame p95
  from about 6 ms to about 30 ms on an M1 Pro: each tile upload advances the
  global publication generation and every local shadow face redraws
  ([ADR-086](../adr/086-world-partition.md#evidence)).

## Goal

An 8 km world that an M1 with 16 GB edits and plays within Bistro's frame
budget, with streaming hitches below one frame at walking and driving speeds,
where imported content streams as well as authored content.

## Proposed work

| Item | Scope | Acceptance evidence |
|---|---|---|
| Hitch budget | Scope local shadow staleness to publications that reach drawn casters (ADR-019 policy), so far tile uploads redraw no face; then budget the remaining per-change costs (mesh re-attachment of every tile, the collision window rebuild) | A matched Release report on M1 Pro: frame p95 within 10% of Bistro alone while gliding at 30 m/s over an 8 km terrain |
| Imported content | Split imported scene documents into cells at import, so Bistro-sized content streams | A converted Bistro renders as before and unloads by cell |
| More containers | Partition the root World and added scenes | Headless runs stream all three container kinds |
| Script state | Keep a behaviour's state across its cell unloading and loading again during a session | A gameplay script in a streamed cell resumes its state after its cell returns |
| Richer proxies | Proxies for cooked meshes and shapes, with baked base colour instead of the brushes' full materials | Proxy memory per cell is bounded and reported |
| Long views | Reversed depth and a far plane that follows the proxy radius | The overview and proxies show at the default camera settings without depth artifacts |
| Project storage | Keep a project scene's cells in the project store beside its revisioned overlays | A managed project saves, reopens and packages its cells |

## Risks

- Streaming moves cost from load time to every frame; budgets that are too
  tight leave holes near fast sources.
- Cell assignment by position can split content a designer sees as one piece;
  children follow their root to limit this.
