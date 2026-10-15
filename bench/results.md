# Benchmarks

## Model speed (llama-bench)

| Machine | Model | Prompt (pp512) | Generation (tg128) |
|---|---|---|---|
| Raspberry Pi 5, CPU, 4 threads | Qwen3-1.7B Q4_K_M | ~48 t/s | ~7.4 t/s |
| PC, RTX 3080 Ti, all layers on GPU | Gemma 4 12B | ~3311 t/s | ~84.7 t/s |

## Gateway overhead

Same tiny request (`max_tokens: 1`), sent directly to llama-server and through Wakey.
Measured from the Pi, 50 runs after 5 warm-up runs, on 2026-10-16
with [`overhead.sh`](overhead.sh).

| Backend | Route | Median | p95 |
|---|---|---|---|
| Pi · Qwen3-1.7B | direct | 108.8 ms | 116.0 ms |
| Pi · Qwen3-1.7B | through Wakey | 110.3 ms | 142.2 ms |
| PC · Gemma 4 12B | direct | 32.1 ms | 32.3 ms |
| PC · Gemma 4 12B | through Wakey | 33.0 ms | 33.4 ms |

**Overhead: +1.5 ms (Pi) and +0.9 ms (PC) at the median.** On a real answer
that takes seconds, that is well under 0.1 %.

The higher p95 on the Pi comes from Wakey sharing the CPU with llama-server,
which uses all 4 cores. On the PC route the Pi's CPU is idle, so the timings stay similar.

## Cold start (`wakeyctl wake pc`, from powered off)

| Date | Boot | Model load | Total |
|---|---|---|---|
| — | 28.6 s | 6.0 s | 34.6 s |
