# Experimental M3 Pro attachment compression

This series adds default-off Honeykrisp compression for legacy G15S/HAL0:
single-sample RGBA16F and R8 color attachments, and the depth plane of
D32_SFLOAT_S8_UINT attachments. Stencil remains uncompressed. Color images
with storage, host-transfer, feedback-loop, sparse, mutable-view or
transfer-destination usage stay uncompressed. The new paths require one
ordinary 2D mip level and array layer.

**Integration dependency:** these patches require the legacy M3 driver at
[`3d7a02dc8d29ab0c23d63d066f1b8f03aa5838b6`](https://github.com/aurora-silicon/mesa-eryk-m4/commit/3d7a02dc8d29ab0c23d63d066f1b8f03aa5838b6).
Omacom's `main` at
`7cda7850edd103ace21aac37d416d2fdf7a282e1` and `26.2` at
`0ae52750c0ccc84fda91b5a45708325b189073c7` do not provide its G15S identity,
HAL parameters or runtime. These are reviewable patches for that driver,
not changes to the current upstream driver. Adding this directory alone
does not change the built driver or enable M3 hardware.

The series ends at tested source
`3c74c2b3130f31cfdf57455097c96966a2487eee`, tree
`3362757e137e676638596fc540c8bd3045926ad3`. The six patches retain their
original authors and Signed-off-by lines. `series.json` gives the ordered
patch hashes and original commit IDs. Apply all six in order to the exact
base; patch 5 supplies the RGBA16F raw-upload exclusion.

| Patch | Behavior |
| --- | --- |
| 1 | Honor explicit DRM modifier plane offsets in requirements, binding and layout queries; validate malformed layouts. |
| 2 | Make the production plane-layout control available to package builds with assertions enabled. |
| 3 | Compress the depth plane of supported split depth/stencil images; keep stencil uncompressed. |
| 4 | Add the narrow RGBA16F attachment option. |
| 5 | Keep raw RGBA16F upload destinations uncompressed. |
| 6 | Add the narrow R8 attachment option. |

The experimental switches are `HK_M3_COMPRESSED_RGBA16F=1`,
`HK_M3_COMPRESSED_R8=1` and `HK_M3_COMPRESSED_SPLIT_DEPTH=1`. All default to
off. They select the guarded paths only after the existing driver has
admitted the GPU; they do not provide device admission or firmware migration.

## Results on the tested driver

At an actual 1920×1080 render size, original vkQuake demo1 with unchanged
game, assets, gamma 0.9 and contrast 1.4:

| Change | Disabled median | Enabled median |
| --- | ---: | ---: |
| RGBA16F attachment compression | 176.2 FPS | 224.9 FPS |
| R8 attachment compression, with RGBA16F already enabled | 223.95 FPS | 230.7 FPS |

The R8 comparison used six runs per condition across two independent boots.
These measurements are separate comparisons, not additive percentages.
The retained complete installation subsequently measured about 228–232 FPS.
The M1 Air reference remains faster; performance parity is not achieved.

The aarch64 package built with LLVM22. Compiler, libasahi and split-depth
controls passed. Hardware controls cover production attachment writes,
sampling, replacement and blending, usage exclusions and all pixels in
105 frames. RGB10 uses the existing one-code-value tolerance; alpha is
exact. Deliberately wrong producer and resolve shaders fail the controls.
The same package's M1 controls pass with the options both off and on.
These results apply to the named driver and package, not an unbuilt port
onto Omacom `main`. M2 and other M3 variants are not hardware-qualified by
these results.

The tested package SHA256 is
`b8b216b7d8c3d29ac232c1ec1e67424b1fe6f01a35634c525ec8b35039fdf45c`;
its Vulkan library SHA256 is
`087c89edb739a232987bccf09a0335336a8439ce5c6f4dfae56aad3d5734ff07`.
The matched kernel is `ac34611f8de50386b6c08872c07f857fdbc24cf2`, retained in
[linux-aurora #93](https://github.com/omacom/linux-aurora/pull/93).
[linux-aurora #94](https://github.com/omacom/linux-aurora/pull/94) records
the experimental Pro profile.

## Integration and remaining limits

First establish a reviewed Omacom branch containing the compatible G15S
runtime and HAL/UAPI contract. Then apply this series as source commits
and repeat its build, usage-exclusion, pixel and M1 regression controls
on that branch before release. Keep the options default-off during review.

Storage images and unsupported raw uploads remain uncompressed. This
series does not implement the runtime, widen device admission, establish
full Vulkan conformance, or provide Vulkan for the M3 Air HAL200 profile.
The native OpenGL desktop driver is separate from this Honeykrisp Vulkan
series.
