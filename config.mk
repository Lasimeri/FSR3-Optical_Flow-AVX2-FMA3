# config.mk: what the build may be told. Each value can be overridden on the command line or from the environment.
HOST_CC ?= cc
# The instruction set the binaries need: x86-64-v3 is AVX2, FMA3, BMI2 and what came before (Haswell and later, Zen and later). Not -march=native: what is built here must run on the laptop too.
ARCH    ?= -march=x86-64-v3
OPT     ?= -O2
JOBS    ?= 8
