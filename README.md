
### OGS Score Estimator Experiment

Quick and dirty proof-of-concept hack to turn pachi into an ogs
[score estimator](https://github.com/online-go/score-estimator).

'make' to build.

The resulting 'estimator' binary should behave the same as ogs estimator.

Usage: `estimator [-n simulations] [-d debug_level] file.game`

For example: `estimator ~/src/score-estimator/test_games/1776378.game`
