#!/bin/sh
# measures the whole path: HTTP parsing, strategy and response.
# usage: scripts/http_load.sh [clients] [seconds] [url]
# needs ab (Apache Bench). these likes are real, so use a throwaway --db.
clients=${1:-16}
seconds=${2:-5}
url=${3:-http://127.0.0.1:8080}

for strategy in write_through mutex_batch atomic_batch sharded_batch; do
    curl -sf -o /dev/null -X PUT "$url/api/strategy" \
         -H 'Content-Type: application/json' -d "{\"strategy\":\"$strategy\"}" || exit 1
    # -l: the count in the response grows, so its length changes
    result=$(ab -q -k -l -c "$clients" -t "$seconds" -n 50000000 -m POST "$url/like" 2>&1)
    echo "$strategy"
    echo "$result" | grep -E 'Requests per second|Failed requests|Non-2xx|  50%|  99%'
done
