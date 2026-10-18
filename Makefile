CXX ?= c++
PKG_CONFIG ?= pkg-config
DOTNET ?= $(if $(wildcard .tools/dotnet/dotnet),.tools/dotnet/dotnet,dotnet)
OSU_DIR ?= $(HOME)/.local/share/osu
INTEGRATION_TIMEOUT ?= 300
INTEGRATION_VERIFY ?= quick
export DOTNET_CLI_TELEMETRY_OPTOUT := 1

PACKAGES := libavformat libavcodec libavutil libswresample json-c sdl2
CPPFLAGS := -Iinclude $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -MMD -MP -pthread
LDLIBS := $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -pthread

ifeq ($(SANITIZE),1)
    CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address,undefined
endif

CORE_OBJECTS := obj/library.o obj/library_loader.o obj/storage_path.o obj/track.o
AUDIO_OBJECTS := obj/audio_probe.o obj/audio_player.o
PLAYER_OBJECTS := obj/main.o obj/play_command.o $(AUDIO_OBJECTS) $(CORE_OBJECTS)
STORAGE_TEST_OBJECTS := obj/storage_path_test.o $(CORE_OBJECTS)
LIBRARY_TEST_OBJECTS := obj/library_test.o $(CORE_OBJECTS)
AUDIO_TEST_OBJECTS := obj/audio_player_test.o $(AUDIO_OBJECTS)
DEPENDENCY_FILES := $(PLAYER_OBJECTS:.o=.d) $(STORAGE_TEST_OBJECTS:.o=.d) $(LIBRARY_TEST_OBJECTS:.o=.d) $(AUDIO_TEST_OBJECTS:.o=.d)

.PHONY: all test clean indexer test-indexer test-integration

all: bin/osu-player bin/storage-path-tests bin/library-tests bin/audio-player-tests

bin/osu-player: $(PLAYER_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/storage-path-tests: $(STORAGE_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/library-tests: $(LIBRARY_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/audio-player-tests: $(AUDIO_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

obj/%.o: src/%.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

obj/%_test.o: tests/%_test.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

bin obj:
	mkdir -p $@

test: bin/osu-player bin/storage-path-tests bin/library-tests bin/audio-player-tests
	./bin/storage-path-tests
	./bin/library-tests
	python3 -m unittest discover -s tests -p 'test_*.py'
	python3 tests/audio_integration.py

indexer:
	$(DOTNET) restore tools/osu-indexer --locked-mode
	$(DOTNET) build tools/osu-indexer --configuration Release --no-restore --nologo

test-indexer:
	$(DOTNET) restore tests/osu-indexer-tests --locked-mode
	$(DOTNET) run --project tests/osu-indexer-tests --configuration Release --no-restore

# Requires lazer to be closed. Does not launch it or write to its directory.
test-integration: bin/osu-player indexer
	python3 tests/indexer_integration.py --dotnet "$(DOTNET)" --osu-dir "$(OSU_DIR)" --timeout "$(INTEGRATION_TIMEOUT)" --verify "$(INTEGRATION_VERIFY)"

clean:
	rm -rf bin obj

-include $(DEPENDENCY_FILES)
