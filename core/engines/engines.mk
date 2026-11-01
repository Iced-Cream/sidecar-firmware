# core/engines/engines.mk - the engines' sources for a non-Arduino build of the core (core/Makefile,
# pi/engine/Makefile). Set ENG to the folder core/engines/setup_engines.sh filled, include this, then use
# ENG_SRC (C++), ENG_C_SRC (C), ENG_INC (include flags) and ENG_FLAGS (defines).
ENG_SRC   := $(addprefix $(ENG)/Plaits/,$(shell cd $(ENG)/Plaits 2>/dev/null && find plaits stmlib -name '*.cc' | sort)) \
             $(wildcard $(ENG)/DaisySP/Filters/*.cpp $(ENG)/DaisySP/Utility/*.cpp)
# ENG_C_SRC: the C sources: mdkit's voices and the MD glue (the built-in MD, core/md/md.h; DIGIDRUM's until 2026-10-29)
ENG_C_SRC := $(wildcard $(ENG)/MDKit/*.c)
ENG_INC   := -I$(ENG)/Plaits -I$(ENG)/Juno -I$(ENG)/DaisySP -I$(ENG)/Sampler -I$(ENG)/MDKit
# stmlib's portable code paths (no Cortex-M ssat/usat asm, no .ramtext section), as sim/plaits/build.sh and spike3
ENG_FLAGS := -DTEST
# The Pi kernel's render loops at -O3 (the rest -O2; docs/superpowers/specs/2026-10-31-neon-pass.md): GCC's -O2 vectoriser
# ("very cheap") leaves any loop it cannot prove costs nothing extra. File by file only where nothing else lives: voices.cpp, poly.cpp
# and inst.cpp stay -O2 (the machines' dispatch, DSP SAMPLE, measured +12 % on the Pi with them at -O3); JUNO's and DAISY's renders
# carry their own RYTM_RENDER_O3. Paths under core/; the engine sources under the engines' folder.
HOT_O3     := fx/insert.cpp
HOT_O3_ENG := Plaits/plaits/dsp/engine/virtual_analog_engine.cc
