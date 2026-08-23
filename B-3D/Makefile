CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra
LDLIBS   = -lm -lpthread -ldl

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)

all: B-3D

B-3D: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): src/b3d.h
src/stb.o: src/stb_image.h
src/gpu.o: src/cl_api.h src/gpu_kernel.h

src/gpu_kernel.h: src/gpu_kernel.cl
	@printf '/* Generated from $< by the Makefile. Do not edit. */\n' > $@
	@printf '#ifndef B3D_GPU_KERNEL_H\n#define B3D_GPU_KERNEL_H\n' >> $@
	@printf 'static const char B3D_CL_SRC[] =\n' >> $@
	@sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< >> $@
	@printf ';\n#endif\n' >> $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJS) src/gpu_kernel.h B-3D

.PHONY: all clean
