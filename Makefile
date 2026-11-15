# Makefile: the optical flow for a host CPU (AVX2 and FMA3), its gate, and the program that doubles a monitor with it.
#   make            everything: out/ofgate (kernels, their lane model, the gate), out/ofgl (the live program), out/moving (test motion)
#   make gate       out/ofgate alone: needs no Wayland, no PipeWire, no GL
include config.mk
MAKEFLAGS += -j$(JOBS) -Oline --no-builtin-rules

GEN := out/gen
INC := -Isrc/common -Isrc/kern -Isrc/engine -Isrc/host -I$(GEN)
WARN := -Wall -Wextra -Werror
# -ffp-contract=off: a*b+c stays two roundings unless the source asks for a fused operation, so the native body and the lane model agree bit for bit.
CFLAGS := -std=gnu11 $(OPT) $(ARCH) $(WARN) -ffp-contract=off -fno-math-errno $(INC) -MMD -MP
PW_CFLAGS := $(patsubst -I%,-isystem %,$(shell pkg-config --cflags libpipewire-0.3 libspa-0.2 2>/dev/null))
PW_LIBS := $(shell pkg-config --libs libpipewire-0.3 2>/dev/null)
WL_LIBS := $(shell pkg-config --libs wayland-client 2>/dev/null)
GL_LIBS := $(shell pkg-config --libs egl glesv2 wayland-egl 2>/dev/null)

PROTOS := presentation-time viewporter fifo-v1 wlr-layer-shell-unstable-v1 xdg-shell zkde-screencast-unstable-v1
PROTO_H := $(PROTOS:%=$(GEN)/%-client.h)
PROTO_O := $(PROTOS:%=out/gen/%-protocol.o)

KERN := framegen fsr3 deblock amdfi img_ref img_vx bgra flow
ENGINE := cpus team engine
KERN_NATIVE := $(KERN:%=out/native/kern/%.o)
KERN_MODEL := $(KERN:%=out/model/kern/%.o)

all: gate out/ofgl out/moving
gate: out/ofgate

out/native/kern/%.o: src/kern/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) -c $< -o $@
out/native/engine/%.o: src/engine/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) -c $< -o $@
out/native/host/%.o: src/host/%.c | $(PROTO_H)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) $(PW_CFLAGS) -c $< -o $@
out/native/tools/%.o: tools/%.c | $(PROTO_H)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) -c $< -o $@

# The lane model of the same kernels, beside the native ones in one program: every global name the native objects define gets the prefix model_, by a header written from their own symbol tables and put before the source.
$(GEN)/model_ns.h: $(KERN_NATIVE)
	@mkdir -p $(dir $@)
	nm -g --defined-only $^ | awk 'NF == 3 && !seen[$$3]++ { print "#define " $$3 " model_" $$3 }' | LC_ALL=C sort > $@
out/model/kern/%.o: src/kern/%.c $(GEN)/model_ns.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) -DVX_MODEL -include $(GEN)/model_ns.h -c $< -o $@

$(GEN)/%-client.h: tools/protocols/%.xml
	@mkdir -p $(dir $@)
	wayland-scanner client-header $< $@
$(GEN)/%-protocol.c: tools/protocols/%.xml
	@mkdir -p $(dir $@)
	wayland-scanner private-code $< $@
out/gen/%-protocol.o: $(GEN)/%-protocol.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O2 -c $< -o $@

out/ofgate: out/native/host/ofgate.o $(KERN_NATIVE) $(KERN_MODEL) out/native/engine/cpus.o out/native/engine/team.o
	$(HOST_CC) -o $@ $^ -lm -lpthread
out/ofgl: out/native/host/ofgl.o out/native/host/wlp.o $(KERN_NATIVE) $(ENGINE:%=out/native/engine/%.o) $(PROTO_O)
	$(HOST_CC) -o $@ $^ $(PW_LIBS) $(WL_LIBS) $(GL_LIBS) -lm -lpthread
out/moving: out/native/tools/moving.o out/gen/viewporter-protocol.o out/gen/wlr-layer-shell-unstable-v1-protocol.o out/gen/xdg-shell-protocol.o
	$(HOST_CC) -o $@ $^ $(WL_LIBS)

.PRECIOUS: $(GEN)/%-protocol.c
.PHONY: all gate
-include $(shell find out -name '*.d' 2>/dev/null)
