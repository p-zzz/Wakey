#!/bin/bash

set -euo pipefail            # stop on errors / unset variables

url=$1                       # e.g. http://127.0.0.1:8095
model=$2                     # qwen3-1.7b or gemma-4-12b
keyfile=${3:-}               # optional
runs=50

auth=()
if [[ -n "$keyfile" ]]; then
    auth=(-H "Authorization: Bearer $(<"$keyfile")")
fi
body='{"model":"'"$model"'","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}'

one_request() {
    curl -s -o /dev/null -w '%{http_code} %{time_total}\n' "${auth[@]}" -H 'Content-Type: application/json' -d "$body" "$url/v1/chat/completions"
}

read -r code _ < <(one_request)
[[ "$code" == 200 ]] || { echo "$url not answering (HTTP $code)" >&2; exit 1; }

for i in {1..5}; do one_request > /dev/null; done      # warm-up

: > times.txt                                           # empty the file
for (( i = 0; i < runs; i++ )); do
    read -r code t < <(one_request)
    if [[ "$code" != 200 ]]; then
        echo "request $i failed: HTTP $code" >&2
        exit 1
    fi
    printf "%s\n" "$t" >> times.txt
done

sort -n times.txt | awk '{a[NR]=$1} END {printf "median %.1f ms   p95 %.1f ms\n", a[int(NR*0.5)+1]*1000, a[int(NR*0.95)]*1000}'
