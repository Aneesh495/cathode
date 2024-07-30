# ==========================================================================
# CATHODE — CPU graphics engine with NTSC/CRT emulation, in your terminal.
# Hand-written AArch64 NEON assembly + dense C/C++. No GPU. No external libs.
# ==========================================================================
CC       := clang
CXX      := clang++
# -MMD -MP emit a .d file of header dependencies next to each .o, so editing a
# header (e.g. changing a struct in a frozen contract) recompiles every TU that
# includes it. Without this, a stale object built against an old struct layout
# silently mismatches the rest of the program (this bit us once: adding a field
# to RasterCtx crashed only the scenes whose .o wasn't rebuilt).
DEPFLAGS := -MMD -MP
CFLAGS   := -O3 -std=c11 -Iinclude -Isrc -Isrc/app -Wall -Wextra \
            -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
            -ffast-math -fno-math-errno -DCATHODE_HAVE_POLYGLOT_SCENES=1 $(DEPFLAGS)
CXXFLAGS := -O3 -std=c++20 -Iinclude -Isrc -Isrc/app -Wall -Wextra \
            -Wno-unused-parameter -ffast-math -fno-math-errno $(DEPFLAGS)
# Opt-in real audio output: `make AUDIO=1` links macOS AudioToolbox and enables
# device playback via cpp_audio_* . Default off keeps the build headless-safe.
AUDIO_LIBS :=
ifdef AUDIO
CXXFLAGS   += -DCATHODE_AUDIO
AUDIO_LIBS := -framework AudioToolbox -framework CoreFoundation
endif
# -lc++ is added implicitly by the clang++ driver used for the final link.
LDFLAGS  := -lm -lpthread $(AUDIO_LIBS)
ASFLAGS  := -Iinclude

BUILD    := build
BIN      := $(BUILD)/bin

# ---- Rust compute core (staticlib linked into the C host) -------------------
RUST_DIR := rustsrc
RUST_LIB := $(RUST_DIR)/target/release/libcathode_rustcore.a

# ---- source groups ----------------------------------------------------------
ASM_SRC  := $(wildcard src/asm/*.s)
CORE_SRC := src/core/simd_ref.c src/core/dsp_ref.c src/core/framebuffer.c \
            src/core/noise.c src/core/image.c src/core/postfx_ref.c \
            src/core/fastmath_ref.c src/core/raykernel_ref.c \
            src/core/fractalkernel.c src/core/fractalkernel_ref.c src/core/gif.c \
            src/core/blur_ref.c src/core/gravkernel_ref.c src/core/wav.c \
            src/core/tracker.c
REND_SRC := src/render/raster.c src/render/mesh.c src/render/sdf.c src/render/crt.c \
            src/render/text.c
PHYS_SRC := src/physics/nbody.c src/physics/fluid.c src/physics/sph.c \
            src/physics/rigidbody.c
TUI_SRC  := src/tui/tui.c
APP_SRC  := src/app/registry.c src/app/threadpool.c
SCENE_SRC:= $(wildcard src/scenes/scene_*.c)
CPP_SRC  := $(wildcard src/cpp/*.cpp)

# Everything except the two entry points (main.c / capture.c).
LIB_SRC  := $(CORE_SRC) $(REND_SRC) $(PHYS_SRC) $(TUI_SRC) $(APP_SRC) $(SCENE_SRC)
LIB_OBJ  := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRC))
ASM_OBJ  := $(patsubst src/%.s,$(BUILD)/%.o,$(ASM_SRC))
CPP_OBJ  := $(patsubst src/%.cpp,$(BUILD)/%.o,$(CPP_SRC))
ALL_LIB  := $(LIB_OBJ) $(ASM_OBJ) $(CPP_OBJ)

# ---- top-level targets ------------------------------------------------------
.PHONY: all clean test tests run capture contact dirs count help rust rust-test cpp
all: dirs rust $(BIN)/cathode $(BIN)/capture
	@echo "==> built $(BIN)/cathode and $(BIN)/capture"

# Rust staticlib via cargo (only rebuilds when rust sources change).
rust: $(RUST_LIB)
$(RUST_LIB): $(wildcard $(RUST_DIR)/src/*.rs) $(RUST_DIR)/Cargo.toml
	cd $(RUST_DIR) && cargo build --release

$(BIN)/cathode: $(ALL_LIB) $(RUST_LIB) $(BUILD)/app/main.o
	$(CXX) $(ALL_LIB) $(BUILD)/app/main.o $(RUST_LIB) -o $@ $(LDFLAGS)

$(BIN)/capture: $(ALL_LIB) $(RUST_LIB) $(BUILD)/app/capture.o
	$(CXX) $(ALL_LIB) $(BUILD)/app/capture.o $(RUST_LIB) -o $@ $(LDFLAGS)

# ---- pattern rules ----------------------------------------------------------
$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pull in the auto-generated header-dependency files that sit next to each *.o,
# so a changed header forces recompilation of everything that includes it. Only
# object-adjacent .d files are included (those whose sibling .o exists); test
# BINARIES are relinked wholesale and must NOT gain headers as `$^` inputs.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null | while read d; do [ -f "$${d%.d}.o" ] && echo "$$d"; done)

# The engine build of the synth links against the Rust staticlib, so it can use
# the O(n log n) FFT for its spectrum. The standalone `test_synth` compiles
# synth.cpp WITHOUT this define (and without libcathode_rustcore.a) and falls
# back to the reference DFT — keeping that test self-contained.
$(BUILD)/cpp/synth.o: CXXFLAGS += -DCATHODE_HAVE_RUST_FFT

$(BUILD)/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

dirs:
	@mkdir -p $(BIN) $(BUILD)/asm $(BUILD)/core $(BUILD)/render \
	          $(BUILD)/physics $(BUILD)/tui $(BUILD)/app $(BUILD)/scenes \
	          $(BUILD)/cpp assets

rust-test:
	cd $(RUST_DIR) && cargo test --release

# C++ subsystem tests (compiled + run standalone).
CPP_TESTS := test_tracer test_synth test_scenegraph test_marchingcubes test_csg test_softbody
cpp-test: dirs
	@for t in $(CPP_TESTS); do \
	  printf "%-18s " $$t; \
	  case $$t in \
	    test_tracer)         src="src/cpp/tracer.cpp src/core/framebuffer.c";; \
	    test_synth)          src="src/cpp/synth.cpp";; \
	    test_scenegraph)     src="src/cpp/scenegraph.cpp";; \
	    test_marchingcubes)  src="src/cpp/marching_cubes.cpp";; \
	    test_csg)            src="src/cpp/csg.cpp";; \
	    test_softbody)       src="src/cpp/softbody.cpp";; \
	  esac; \
	  if $(CXX) $(filter-out $(DEPFLAGS),$(CXXFLAGS)) test/$$t.cpp $$src -o $(BIN)/$$t -lm 2>$(BUILD)/$$t.build.log \
	     && $(BIN)/$$t > $(BUILD)/$$t.log 2>&1; then echo "PASS"; \
	  else echo "FAIL (see $(BUILD)/$$t.log)"; fi; \
	done

# Golden-image regression: render every scene deterministically, hash it, and
# compare to test/golden.txt. Needs the whole engine linked (all scenes +
# subsystems), so it reuses the library objects. `make golden-update` rewrites
# the baselines after an intentional visual change.
$(BIN)/test_golden: test/test_golden.c $(ALL_LIB) $(RUST_LIB) | dirs
	$(CC) $(CFLAGS) -c test/test_golden.c -o $(BUILD)/test_golden.o
	$(CXX) $(BUILD)/test_golden.o $(ALL_LIB) $(RUST_LIB) -o $@ $(LDFLAGS)
golden: $(BIN)/test_golden
	$(BIN)/test_golden
golden-update: $(BIN)/test_golden
	$(BIN)/test_golden update

# End-to-end integration test: like the golden harness it links the whole
# engine (all scenes + subsystems) and runs the full scene→CRT→encode chain,
# validating the emitted PNG/GIF/WAV bytes.
$(BIN)/test_integration: test/test_integration.c $(ALL_LIB) $(RUST_LIB) | dirs
	$(CC) $(CFLAGS) -c test/test_integration.c -o $(BUILD)/test_integration.o
	$(CXX) $(BUILD)/test_integration.o $(ALL_LIB) $(RUST_LIB) -o $@ $(LDFLAGS)
integration: $(BIN)/test_integration
	$(BIN)/test_integration

# Full cross-language suite: C + Rust + C++ + golden regression + integration.
test-all: test rust-test cpp-test golden integration
	@echo "==> full polyglot suite complete (C + Rust + C++ + golden)"

# ---- unit tests -------------------------------------------------------------
TESTS := test_simd test_dsp test_noise test_image test_raster test_sdf \
         test_crt test_nbody test_fluid test_tui test_postfx \
         test_fastmath test_raykernel test_fractalkernel test_gif test_properties \
         test_blur test_sph test_project test_render_props test_gravkernel \
         test_text test_wav test_tracker test_rigidbody

test_simd_OBJ    := src/asm/simd_neon.s src/core/simd_ref.c
test_dsp_OBJ     := src/asm/dsp_neon.s src/core/dsp_ref.c
test_postfx_OBJ  := src/asm/postfx_neon.s src/core/postfx_ref.c
test_fastmath_OBJ:= src/asm/fastmath_neon.s src/core/fastmath_ref.c
test_raykernel_OBJ:= src/asm/raykernel_neon.s src/core/raykernel_ref.c
test_fractalkernel_OBJ:= src/asm/fractalkernel_neon.s src/core/fractalkernel.c src/core/fractalkernel_ref.c
test_gif_OBJ     := src/core/gif.c src/core/framebuffer.c
test_properties_OBJ := src/asm/simd_neon.s src/core/simd_ref.c src/core/dsp_ref.c src/core/noise.c src/physics/nbody.c src/core/framebuffer.c
test_blur_OBJ    := src/asm/blur_neon.s src/core/blur_ref.c
test_sph_OBJ     := src/physics/sph.c src/core/framebuffer.c
test_project_OBJ := src/asm/project_neon.s src/core/simd_ref.c
test_render_props_OBJ := src/render/raster.c src/render/mesh.c src/render/sdf.c src/render/crt.c src/core/framebuffer.c src/core/noise.c src/core/dsp_ref.c src/asm/simd_neon.s src/core/simd_ref.c src/asm/dsp_neon.s
test_gravkernel_OBJ := src/asm/gravkernel_neon.s src/core/gravkernel_ref.c
test_text_OBJ   := src/render/text.c src/core/framebuffer.c
test_wav_OBJ    := src/core/wav.c
test_tracker_OBJ:= src/core/tracker.c
test_rigidbody_OBJ := src/physics/rigidbody.c
test_noise_OBJ := src/core/noise.c
test_image_OBJ := src/core/image.c src/core/framebuffer.c
test_raster_OBJ:= src/render/raster.c src/render/mesh.c src/core/framebuffer.c src/core/noise.c src/asm/simd_neon.s src/core/simd_ref.c
test_sdf_OBJ   := src/render/sdf.c src/core/framebuffer.c
test_crt_OBJ   := src/render/crt.c src/asm/dsp_neon.s src/core/dsp_ref.c src/core/framebuffer.c
test_nbody_OBJ := src/physics/nbody.c src/core/framebuffer.c src/core/noise.c
test_fluid_OBJ := src/physics/fluid.c src/core/framebuffer.c
test_tui_OBJ   := src/tui/tui.c src/core/framebuffer.c

# Tests compile all their sources in a single clang invocation (link at once).
# That is incompatible with -MMD -MP (clang refuses -o with multiple outputs),
# so strip DEPFLAGS here — tests are rebuilt wholesale anyway.
define TEST_template
$(BIN)/$(1): test/$(1).c $$($(1)_OBJ) | dirs
	$$(CC) $$(filter-out $$(DEPFLAGS),$$(CFLAGS)) $$($(1)_CFLAGS) $$^ -o $$@ $$(LDFLAGS)
endef
# Escape-time fractals are chaotic: the NEON kernel and its C reference must use
# bit-identical (non-fused, non-fast) float rounding, or boundary points'
# iteration counts diverge. Build this test strict-FP so the equivalence holds.
test_fractalkernel_CFLAGS := -fno-fast-math -ffp-contract=off
$(foreach t,$(TESTS),$(eval $(call TEST_template,$(t))))

tests: dirs $(addprefix $(BIN)/,$(TESTS))
	@echo "==> built all unit tests"

test: tests
	@echo "=================== CATHODE TEST SUITE ==================="
	@pass=0; fail=0; \
	for t in $(TESTS); do \
	  printf "%-14s " $$t; \
	  if $(BIN)/$$t > $(BUILD)/$$t.log 2>&1; then echo "PASS"; pass=$$((pass+1)); \
	  else echo "FAIL (see $(BUILD)/$$t.log)"; fail=$$((fail+1)); fi; \
	done; \
	echo "=========================================================="; \
	echo "passed=$$pass failed=$$fail"; \
	[ $$fail -eq 0 ]

# ---- convenience ------------------------------------------------------------
bench: dirs
	$(CC) $(filter-out $(DEPFLAGS),$(CFLAGS)) test/bench_all.c \
	    src/asm/simd_neon.s src/core/simd_ref.c src/asm/dsp_neon.s src/core/dsp_ref.c \
	    src/asm/fastmath_neon.s src/core/fastmath_ref.c \
	    src/asm/raykernel_neon.s src/core/raykernel_ref.c \
	    src/asm/blur_neon.s src/core/blur_ref.c \
	    src/asm/fractalkernel_neon.s src/core/fractalkernel.c src/core/fractalkernel_ref.c \
	    -o $(BIN)/bench_all $(LDFLAGS)
	$(BIN)/bench_all

run: all
	$(BIN)/cathode

capture: all
	$(BIN)/capture --all 90 assets 320 240

contact: all
	$(BIN)/capture --all 120 assets 480 360

# self-running demo reel: every scene, crossfading, as one animated GIF
reel: all
	$(BIN)/capture --reel assets/cathode_reel.gif 50 18 320 240
	@echo "==> wrote assets/cathode_reel.gif"

count:
	@echo "Lines of code by language:"; \
	echo -n "  C/headers : "; find src include -name '*.c' -o -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}'; \
	echo -n "  Assembly  : "; find src -name '*.s' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}'; \
	echo -n "  Total     : "; find src include test -name '*.c' -o -name '*.h' -o -name '*.s' | xargs cat 2>/dev/null | wc -l

clean:
	rm -rf $(BUILD)

help:
	@echo "make all      - build cathode + capture"
	@echo "make test     - build & run the full unit-test suite"
	@echo "make run      - build & launch the interactive demo"
	@echo "make capture  - render all scenes to PNGs in assets/"
	@echo "make count    - count lines of code"
