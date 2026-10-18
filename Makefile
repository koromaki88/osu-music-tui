CXX ?= c++
PKG_CONFIG ?= pkg-config
DOTNET ?= $(if $(wildcard .tools/dotnet/dotnet),.tools/dotnet/dotnet,dotnet)
OSU_DIR ?= $(HOME)/.local/share/osu
INTEGRATION_TIMEOUT ?= 300
INTEGRATION_VERIFY ?= quick
TEST_LOG_DIR ?= artifacts
RELEASE_DIR ?= artifacts/release
RELEASE_CXXFLAGS ?= -O2 -DNDEBUG
CONTAINER_ENGINE ?= docker
export DOTNET_CLI_TELEMETRY_OPTOUT := 1

PACKAGES := libavformat libavcodec libavutil libswresample json-c sdl2 ncursesw
CPPFLAGS = -Iinclude $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -MMD -MP -pthread
LDLIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -pthread

ifeq ($(SANITIZE),1)
    CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address,undefined
endif

CORE_OBJECTS := obj/library.o obj/library_loader.o obj/storage_path.o obj/track.o obj/player_controller.o obj/library_view.o obj/terminal_text.o obj/library_refresh.o
AUDIO_OBJECTS := obj/audio_probe.o obj/audio_player.o
PLAYER_OBJECTS := obj/main.o obj/play_command.o obj/tui_command.o $(AUDIO_OBJECTS) $(CORE_OBJECTS)
STORAGE_TEST_OBJECTS := obj/storage_path_test.o $(CORE_OBJECTS)
LIBRARY_TEST_OBJECTS := obj/library_test.o $(CORE_OBJECTS)
AUDIO_TEST_OBJECTS := obj/audio_player_test.o $(AUDIO_OBJECTS)
CONTROLLER_TEST_OBJECTS := obj/player_controller_test.o $(CORE_OBJECTS)
TUI_TEST_OBJECTS := obj/tui_model_test.o $(CORE_OBJECTS)
DEPENDENCY_FILES := $(PLAYER_OBJECTS:.o=.d) $(STORAGE_TEST_OBJECTS:.o=.d) $(LIBRARY_TEST_OBJECTS:.o=.d) $(AUDIO_TEST_OBJECTS:.o=.d) $(CONTROLLER_TEST_OBJECTS:.o=.d) $(TUI_TEST_OBJECTS:.o=.d)

.PHONY: all test clean indexer test-indexer test-integration _test _test-indexer _test-integration release appimage appimage-local test-appimage _test-appimage arch-package test-arch-package _test-arch-package

all: bin/osu-player bin/storage-path-tests bin/library-tests bin/audio-player-tests bin/player-controller-tests bin/tui-model-tests

# Separate optimized build: never reuse ordinary/sanitized objects for releases.
RELEASE_SOURCES := $(wildcard src/*.cpp)
release: $(RELEASE_DIR)/osu-player

$(RELEASE_DIR)/osu-player: $(RELEASE_SOURCES) $(wildcard include/osu_player/*.hpp) Makefile
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) -std=c++20 $(RELEASE_CXXFLAGS) -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -pthread $(RELEASE_SOURCES) $(LDFLAGS) $(LDLIBS) -o "$@"

appimage:
	python3 tools/build_appimage.py --engine "$(CONTAINER_ENGINE)"

appimage-local: release
	python3 tools/package_appimage.py --player-binary "$(RELEASE_DIR)/osu-player" --dotnet "$(DOTNET)"

arch-package:
	@mkdir -p "$(TEST_LOG_DIR)"
	@bash -o pipefail -c 'python3 tools/build_arch_package.py --dotnet "$(DOTNET)" 2>&1 | tee "$$1"' _ "$(TEST_LOG_DIR)/arch-package.log"

bin/osu-player: $(PLAYER_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/storage-path-tests: $(STORAGE_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/library-tests: $(LIBRARY_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/audio-player-tests: $(AUDIO_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/player-controller-tests: $(CONTROLLER_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(shell $(PKG_CONFIG) --libs json-c) -pthread -o $@

bin/tui-model-tests: $(TUI_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(shell $(PKG_CONFIG) --libs json-c) -pthread -o $@

obj/%.o: src/%.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

obj/%_test.o: tests/%_test.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

bin obj:
	mkdir -p $@

# Wrap the entire inner build/run so compiler errors are logged too. pipefail
# preserves test/build failures (and tee errors) instead of returning tee's success.
test test-indexer test-integration test-appimage test-arch-package:
	@mkdir -p "$(TEST_LOG_DIR)"
	+@bash -o pipefail -c '$(MAKE) --no-print-directory _$@ 2>&1 | tee "$$1"' _ "$(TEST_LOG_DIR)/$(if $(filter test,$@),tests,$@).log"

_test: bin/osu-player bin/storage-path-tests bin/library-tests bin/audio-player-tests bin/player-controller-tests bin/tui-model-tests
	./bin/storage-path-tests
	./bin/library-tests
	./bin/player-controller-tests
	./bin/tui-model-tests
	python3 -m unittest discover -s tests -p 'test_*.py'
	python3 tests/audio_integration.py
	python3 tests/tui_integration.py
	python3 tests/refresh_integration.py

indexer:
	$(DOTNET) restore tools/osu-indexer --locked-mode
	$(DOTNET) build tools/osu-indexer --configuration Release --no-restore --nologo

_test-indexer:
	$(DOTNET) restore tests/osu-indexer-tests --locked-mode
	$(DOTNET) run --project tests/osu-indexer-tests --configuration Release --no-restore

# Requires lazer to be closed. Does not launch it or write to its directory.
_test-integration: bin/osu-player indexer
	python3 tests/indexer_integration.py --dotnet "$(DOTNET)" --osu-dir "$(OSU_DIR)" --timeout "$(INTEGRATION_TIMEOUT)" --verify "$(INTEGRATION_VERIFY)"

_test-appimage: appimage

_test-arch-package: arch-package test-indexer
	python3 tests/arch_package_integration.py --dotnet "$(DOTNET)"

clean:
	rm -rf bin obj

-include $(DEPENDENCY_FILES)
