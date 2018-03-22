
### OGS Score Estimator Experiment

Quick and dirty proof-of-concept hack to turn pachi into an ogs estimator.

'make' to build.
The resulting 'estimator' binary should be ok to use as drop-in replacement
for server-side scoring.

Usage: `estimator [-n simulations] [-d debug_level] file.game`

For example: `estimator t-ogs/game/1776378.game`


### Results

Running ogs estimator test suite: [t-ogs/results](t-ogs/results?raw=true)


