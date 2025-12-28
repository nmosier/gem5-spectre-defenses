#!/bin/bash

run() {
    awk '
BEGIN { base = 0}
{
  if (base == 0) {
    base = $4;
  }
  printf "%.4f\n", $4 / base;
}' "$@"
}

printf "EXPECTED\tACTUAL\n"
paste <(run expected.txt) <(run actual.txt)
