# What's dynamic, what's baked into a PSO, and why it decided the architecture

The brief's second bullet asks for "understanding what state is dynamic vs baked
into a PSO". That distinction is not a detail here — it is the reason this
project routes per-object IDs through Unreal's CustomDepth pass instead of
writing them itself, and the reason the ID channel is 8 bits wide.

## The split

In D3D12, `ID3D12PipelineState` is immutable and created up front. It bakes:

- shaders, root signature layout, input layout
- blend state, rasterizer state
- **the entire depth-stencil description** — `StencilEnable`, `StencilReadMask`,
  `StencilWriteMask`, and the front/back `StencilFailOp` / `StencilDepthFailOp` /
  `StencilPassOp` / `StencilFunc`
- RTV/DSV formats, sample count

What stays dynamic, set per command list:

- `OMSetRenderTargets` — which targets are bound
- `OMSetStencilRef` — **the stencil reference value**
- `ResourceBarrier`, `ClearDepthStencilView`
- viewports, scissors, blend factor, root arguments, vertex/index buffers

The one piece of stencil behaviour that is *not* baked is the reference value.
Everything that decides whether a write happens at all — enable, write mask,
pass op — is frozen into the PSO at creation.

## Why that killed the obvious approach

The naive plan for per-pixel object IDs is: hook every draw, set a per-object
value, let it land in a stencil or ID buffer.

That cannot work from an injected DLL. To make a draw write an arbitrary ID you
need `StencilEnable = TRUE`, a non-zero `StencilWriteMask`, and
`StencilPassOp = REPLACE`. All three are in the PSO. The game created its PSOs
at load time with its own depth-stencil state, and most of them do not write
stencil at all.

Changing that means **creating new PSOs** — one per distinct combination of
shaders, root signature, input layout, blend and raster state the game uses.
A shipped UE title has thousands. You would have to intercept `SetPipelineState`,
look up or build a stencil-writing twin of whatever PSO was bound, and swap it,
while keeping every other piece of state byte-identical so the frame still
renders correctly. That is not instrumentation any more; it is a shadow renderer,
and every mismatch is a visual artifact in someone's game.

Setting only `OMSetStencilRef` per draw — the one dynamic knob — accomplishes
nothing on its own. The reference value is consumed by the compare and pass
operations, and if the bound PSO has `StencilEnable = FALSE`, it is simply
ignored.

## What we did instead

Let the engine own the PSO problem.

UE's CustomDepth pass already creates PSOs with stencil writes enabled, and
already sets `OMSetStencilRef` per primitive from the primitive's
`CustomDepthStencilValue`. So the intervention needed is not a graphics
intervention at all — it is setting an integer property on a game object and
letting the engine's existing pass do the rendering.

That is why the mutation in this project happens through
`UPrimitiveComponent::SetRenderCustomDepth` on the game thread rather than
anywhere in the D3D12 layer. The renderer hooks stay strictly read-only: no hook
alters an argument, and no hook issues GPU work.

**And it is why the ID is 8 bits.** The width is not a shortcut — it is the width
of the stencil channel of the pass the engine already runs. Getting a wider ID
means owning the PSO, which means the shadow renderer above. The 255-slot
constraint that the identity design works around is structural, imposed by where
the boundary between "instrument" and "rewrite" falls.

## What the census can and cannot see, because of this

We hook eight vtable slots:

| hook | why |
|---|---|
| `Present` | frame boundary, capture point |
| `ExecuteCommandLists` | the command queue is not reachable from the swapchain |
| `CreateRenderTargetView` / `CreateDepthStencilView` | descriptor handle → resource |
| `CreateCommittedResource` | initial resource state, which no barrier announces |
| `ResourceBarrier` | shadow resource state so a copy transition is correct |
| `OMSetRenderTargets` | which targets are bound, and in what order |
| `ClearDepthStencilView` | which targets are cleared each frame |

We deliberately do **not** hook `SetPipelineState`. A consequence worth stating
plainly: the census knows *which* depth target was bound and *how often*, and
that it was cleared — but it does not know what stencil operation the draws
against it were configured to perform, because that lives in the PSO.

This is exactly why the election scores on bind count, clear count, format,
dimensions and persistence rather than on "does this pass write stencil". The
signals it uses are the ones actually observable from dynamic state. An election
that keyed on PSO contents would need a ninth hook and a PSO-desc cache, and
would still not beat "cleared every frame, receives almost no draws" as a
signature for CustomDepth.

## Where it showed up as a bug

The same fact bit during the day-1 RenderDoc analysis. Sampling 12 of 1,365
draws reported "no stencil writes anywhere", which was wrong. Depth-stencil
state is per-PSO, not per-draw, so the correct unit of inspection is the
pipeline object: grouping draws by `pipelineResourceId` and inspecting each
distinct PSO once is **exhaustive** and cheap, where sampling draws is neither.
See `DEBUGGING.md` §2.

## Postscript: the copy path is unaffected

Readback never touches a PSO. `CopyTextureRegion` is not a draw — it needs no
pipeline state, only correct resource states on both sides, which is what the
`ResourceBarrier` shadow provides. That is a large part of why the capture path
could stay read-only with respect to the game's rendering while still doing real
GPU work on its queue.
