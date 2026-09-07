# CIS 7000 warm-up -- build the benchmark programs for Parts 1, 2 and 4.
#
# membw is built for SSE2 only, never AVX2. On Broadwell-DE, AVX2 stores
# raise the core's power license and drop package frequency, which would slow
# the victim by a mechanism unrelated to memory contention. 16 B/store at 2 GHz
# needs only ~0.28 stores/cycle to saturate the memory controller.

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
LDLIBS_MEMBW := -lpthread

BINS := rtt/echo_server rtt/echo_client antagonist/membw rdma/rdma_pingpong

all: $(BINS)

rtt/echo_server: rtt/echo_server.c
	$(CC) $(CFLAGS) -o $@ $<

rtt/echo_client: rtt/echo_client.c
	$(CC) $(CFLAGS) -o $@ $<

antagonist/membw: antagonist/membw.c
	$(CC) $(CFLAGS) -msse2 -o $@ $< $(LDLIBS_MEMBW)

# Needs libibverbs (apt install libibverbs-dev). Built last so a node without
# the RDMA stack still gets the other tools from `make -k`.
rdma/rdma_pingpong: rdma/rdma_pingpong.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# `make figures` must regenerate every report figure from the raw CSVs in
# $(DATA). Set PLOT to your plotting program (see Submission).
# The default assumes code/plot.py takes <data-dir> and <out-dir>. Override
# PLOT if you use another language or path, e.g.
#   make figures PLOT='Rscript code/plot.R'
DATA    ?= data
FIGURES ?= figures
PLOT    ?= python3 code/plot.py

figures:
	@mkdir -p $(FIGURES)
	$(PLOT) $(DATA) $(FIGURES)

clean:
	rm -f $(BINS)

clean-figures:
	rm -f $(FIGURES)/*.pdf

.PHONY: all clean figures clean-figures
