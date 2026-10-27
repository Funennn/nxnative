# Experimental iOS 18 compatibility

This branch targets iOS 18.0 and later, including 18.5, on an iPhone 15 Pro or
Pro Max (A17 Pro). This is an experimental port, not a claim of tested gameplay.
No iPhone build or device gameplay has been validated yet. An unsigned IPA still
requires the user's own game files and the usual build tools on a Mac or CI host.

## Renderer selection

The host selects the classic Metal command backend below iOS 26. iOS 26 and
macOS 26 keep the existing Metal 4 backend. `DM_METAL_BACKEND=metal3` forces the
compatibility backend on a newer system for testing; `metal4` selects the
original backend. This setting belongs in the game's `Documents/env.txt` on iOS.
It does not make Metal 4 available on an older system.

The compatibility layer translates direct argument bindings, GPU-address
indirect draws, compute dispatches, buffer/texture copies, completion handlers,
shared events and presentation to classic Metal. Existing shader argument-block
layouts are preserved. Resources reached indirectly are declared conservatively.
Metal 4 pipeline specialization, pipeline archives, Metal 4 GPU counters and
Metal 4 MetalFX scaling are disabled on this backend; ordinary pipelines and
bilinear presentation remain available.

Distinct GPU mappings of the same physical pages require a completion boundary
on classic Metal: encoder fences alone failed the alias-copy regression. The
compatibility backend finishes the affected command buffer before consuming
aliased storage. Ordinary buffer copies retain asynchronous submission. This is
a correctness fallback for a platform limit and may be slow, particularly with
sparse guest memory or placement heaps. Game performance is unmeasured.

The iOS project and source compile check target A17 rather than A18. Lowering the
OS requirement does not add BC texture support to older iPhones.

## Build without local Xcode

The `iOS 18 compatibility (experimental)` GitHub Actions workflow uses a hosted
macOS runner with Xcode. Push this branch to a repository you control, enable
Actions if GitHub asks, and run the workflow. Its two matrix entries compile all
iPhone host sources and link the production app with minimum OS versions 18.0
and 18.5. The runner still needs an SDK new enough to compile the retained Metal
4 path. There is no need to install an iOS 18 simulator.

Only logs are uploaded. The link fixture contains synthetic modules, not a
playable game, and must not be installed or presented as a game IPA. A green
workflow establishes source compilation and linking, not iPhone runtime support.
No game files or keys are uploaded by this workflow.

## Local Metal checks without full Xcode

On an Apple-silicon Mac whose Command Line Tools include the Metal headers:

```sh
python3 scripts/check_ios18_metal3.py --output /path/to/new/test-logs
```

This builds the six test binaries, runs all eight cases with Metal validation
in direct and deferred encoding modes, and writes logs and a JSON report.
These synthetic GPU tests contain no game data. Real game regression and iPhone
18.5 testing are still required before calling the port supported.
