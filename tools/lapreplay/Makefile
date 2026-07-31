# Host build of the lap-detection replay harness.
# Compiles the real lib/LapManager against a shimmed Arduino.h — no PlatformIO,
# no hardware, nothing that touches the firmware build.
#
#   make && ./replay GH011973.csv -23.60488969 -46.83622658 -23.60493793 -46.83641597
#
# make VERBOSE=1   surfaces LapManager's own log_d() gate diagnostics.

ROOT    := ../..
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra \
            -Ishim -I$(ROOT)/lib/LapManager -I$(ROOT)/lib/Shared

ifdef VERBOSE
CXXFLAGS += -DREPLAY_VERBOSE=1
endif

replay: replay.cpp $(ROOT)/lib/LapManager/LapManager.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f replay

.PHONY: clean
