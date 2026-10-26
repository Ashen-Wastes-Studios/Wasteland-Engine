# The neural-rendering dataset: what it is, and how to rebuild it

This directory contains the complete, clean-room neural-rendering pipeline for Wasteland's
Nova renderer: the dataset exporter, the model, the trainer, the weight exporter, and the
evaluation harness. Nothing here is a black box — the intent is that a developer who has never
seen this project can rebuild the dataset, retrain the network from scratch, and verify that
the result matches what ships.

**License: Apache 2.0**, the same as the rest of the repository (`/LICENSE.txt`). That covers
this code, the trained weights, and the generated dataset. There is no separate or more
restrictive licence on the AI assets.

---

## Why the 1.4 GB of images is not in this repository

The two generated sets are large:

| set | size | purpose |
|---|---|---|
| `dataset/` | ~700 MB | converged ground truth + noisy input pairs |
| `dataset_noisy/` | ~703 MB | the set the shipped network was trained on |

Committing 1.4 GB to git would make every clone pay for it forever, and git stores binaries
badly. So the images are **generated, not stored** — and the generator is deterministic and
fully specified, which is what actually matters for replication.

What *is* committed is the thing that makes regeneration exact: **the manifests.**

```
manifests/dataset.manifest.json
manifests/dataset_noisy.manifest.json
```

Each manifest records, for every one of the 72 captures:

- the scene path and the camera (`name`, `focal`, `distance`, `pitch`, `yaw`)
- `pass_name`, `render_scale`, `quality_preset`, `neural`, `accumulate`, `frame_index`
- `width`, `height`, and the number of `attempts`
- the **`sha256` of the produced image**
- quality gates: `mean_luma`, `contrast_p99`, `blank`

It also pins the `engine` binary by SHA-256 and the capture `config`
(`converged_frame`, `noisy_frame`, `min_contrast_p99`).

So the dataset is not merely documented — it is **byte-verifiable**. Regenerate it, hash the
output, compare against the manifest.

---

## Rebuilding the dataset

From the repository root:

```bash
# See every capture that would run, without touching anything.
python tools/train_neural/export_dataset.py --plan

# Produce the full set (gt + input per scene, camera framed per scene).
python tools/train_neural/export_dataset.py

# Check a produced set against its manifest.
python tools/train_neural/export_dataset.py --validate
```

Requirements are in `README.md` § Requirements. The capture harness drives
`DemonCore-Editor.exe` headlessly, so the engine must be built first — and the manifest pins
which build produced the shipped set. If your engine binary has a different SHA-256, expect
different pixels; that is the point of recording it.

### Verifying a rebuild

The `sha256` per frame is the acceptance test. A rebuild that matches every hash is
bit-identical to the set the network was trained on. A rebuild that differs in a few frames
usually means a different engine build or a driver difference in the capture path — check the
`engine.sha256` field first.

---

## Retraining

The full procedure is in `README.md` § Train. In short:

```bash
python tools/train_neural/train.py       # ~3 s/epoch, ~200 epochs
python tools/train_neural/export_weights.py   # emits the .npz the shaders consume
python tools/train_neural/test_model.py       # evaluation
```

`dataset_stats.json` holds the per-feature standardisation constants. **These are part of the
ABI** — the GLSL and HLSL shader twins apply the same affine before the first matmul. If you
retrain, you must regenerate this file and update the shaders together, or the image will be
wrong in a way that looks like a broken network rather than a mismatched constant.

The HLSL twin serves DX11, DX12 and Vulkan; the GLSL twin serves the OpenGL path. Both must be
kept in step. `tools/verify_nova_glsl.py` checks that they agree.

---

## Prebuilt data

If you would rather not run a 700 MB capture, the generated sets can be published to a dataset
host (Hugging Face Hub is the obvious candidate) with the manifests above as the integrity
record. That is a publishing step, not a licensing one — the licence is already Apache 2.0, so
redistribution of the data is permitted either way.

---

## A note on the boundary

`README.md` § "Why this is the clean-room boundary" explains the constraint this pipeline was
built under. It matters for anyone reusing this work: the network and the training code here
are original, written against the published description of neural denoising rather than derived
from any vendor SDK. Keep it that way if you fork this — the value of the boundary is that the
provenance is answerable.
