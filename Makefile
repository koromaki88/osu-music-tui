CXX ?= c++
PKG_CONFIG ?= pkg-config

CPPFLAGS := -Iinclude $(shell $(PKG_CONFIG) --cflags libavformat libavcodec libavutil json-c)
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -MMD -MP
LDLIBS := $(shell $(PKG_CONFIG) --libs libavformat libavcodec libavutil json-c)

ifeq ($(SANITIZE),1)
    CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address,undefined
endif

CORE_OBJECTS := obj/library.o obj/library_loader.o obj/storage_path.o obj/track.o
PLAYER_OBJECTS := obj/main.o obj/audio_probe.o $(CORE_OBJECTS)
STORAGE_TEST_OBJECTS := obj/storage_path_test.o $(CORE_OBJECTS)
LIBRARY_TEST_OBJECTS := obj/library_test.o $(CORE_OBJECTS)
DEPENDENCY_FILES := $(PLAYER_OBJECTS:.o=.d) $(STORAGE_TEST_OBJECTS:.o=.d) $(LIBRARY_TEST_OBJECTS:.o=.d)

.PHONY: all test clean

all: bin/osu-player bin/storage-path-tests bin/library-tests

bin/osu-player: $(PLAYER_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/storage-path-tests: $(STORAGE_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/library-tests: $(LIBRARY_TEST_OBJECTS) | bin
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

obj/%.o: src/%.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

obj/%_test.o: tests/%_test.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

bin obj:
	mkdir -p $@

test: bin/storage-path-tests bin/library-tests
	./bin/storage-path-tests
	./bin/library-tests

clean:
	rm -rf bin obj

-include $(DEPENDENCY_FILES)
