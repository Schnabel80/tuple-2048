# tuple-2048 – trained model

This branch only holds the trained 4×6-tuple network after 1,000,000 training games
(optimistic start 40 000, TC learning from 150 000 games; avg. score 160k greedy, 252k with 2-layer expectimax).

Weights format v2 incl. visit counters and TC state, xz-compressed and split into < 100 MB parts
(GitHub's file size limit). Reassemble and use:

```bash
git fetch origin model-weights
git checkout origin/model-weights -- 'showcase_1M.bin.xz.part*' SHA256SUMS
cat showcase_1M.bin.xz.part* > showcase_1M.bin.xz && sha256sum -c SHA256SUMS
xz -d showcase_1M.bin.xz            # -> showcase_1M.bin (1,073,741,940 bytes)
./t2048 eval --weights showcase_1M.bin --games 1000 --depth 1 --threads 4
```
