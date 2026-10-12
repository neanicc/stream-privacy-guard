# Stream Privacy Guard

An OBS Studio filter that blurs sensitive things on a live source before they air:
faces, strings you put on a watch-list (your real name, your address, alt
accounts), and structured personal data such as phone numbers, emails and card
numbers.

It is a **safety net, not a guarantee**. A short lookahead delay lets the
detector catch things before the frame is encoded, masks are applied retroactively
to every buffered frame, and when the pipeline cannot keep up it fails closed by
blurring the whole source. Some things will still get through; the eval in this
repo measures how often.

> Status: pre-alpha. The repository currently holds the plan, agent workflows,
> CI structure and eval targets. Build instructions arrive with SPG-01.

## How it works

1. The filter reads back a downscaled copy of each frame.
2. Worker threads run a face detector (YuNet) and a text detector and recognizer
   (PP-OCR via ONNX Runtime).
3. A policy engine fuzzy-matches recognized text against your watch-list and
   checks it against PII patterns.
4. A tracker keeps masks on moving content and holds them briefly after the last
   detection.
5. Full-resolution frames wait in a lookahead buffer; masks found later are
   applied to earlier buffered frames, then a GPU effect blurs the masked regions.

Details: [docs/plan.md](docs/plan.md). Accuracy targets and how they are
measured: [docs/eval.md](docs/eval.md) and [eval/thresholds.toml](eval/thresholds.toml).

## Privacy

Runs entirely on your machine. No accounts, telemetry or network calls at
runtime. Watch-list entries are never logged. Models are downloaded once from
pinned URLs and verified by SHA-256.

## How this repository is built

Most code here is written by Claude Code agents working through GitHub issues
and pull requests. Each PR gets CI on Linux, macOS and Windows, a Claude staff
review against [AGENTS.md](AGENTS.md), and a Greptile review. A merge gate
decides what lands; a human owns the thresholds, CI workflows and live testing.
See [docs/autopilot.md](docs/autopilot.md) for the workflow and
[docs/agent-log.md](docs/agent-log.md) for the decision history.

## License

GPL-2.0, because OBS plugins link against libobs. See [LICENSE](LICENSE).
