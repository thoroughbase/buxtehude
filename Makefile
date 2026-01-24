BUILD_DIR := build
OUTPUT_DIR := out
BUXTEHUDE_INCLUDE_DIR := include
INCLUDE_DIRS := $(BUXTEHUDE_INCLUDE_DIR)
INCLUDE := $(addprefix -I,$(INCLUDE_DIRS))
LIBRARIES := -lfmt -levent_core -levent_pthreads
CXX := clang++
CXXFLAGS := -Wall -std=c++20
CPPFLAGS := $(INCLUDE) -MMD -MP

# library
BUXTEHUDE_DYNAMIC_TARGET := $(OUTPUT_DIR)/libbuxtehude.dylib
BUXTEHUDE_STATIC_TARGET := $(OUTPUT_DIR)/libbuxtehude.a
BUXTEHUDE_SOURCE := $(wildcard src/*.cpp)
BUXTEHUDE_OBJECTS := $(BUXTEHUDE_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
BUXTEHUDE_DEPENDENCIES := $(BUXTEHUDE_OBJECTS:%.o=%.d)

# tests (stream)
TEST_STREAM_TARGET := $(OUTPUT_DIR)/stream-test
TEST_STREAM_SOURCE := tests/stream-test.cpp
TEST_STREAM_OBJECTS := $(TEST_STREAM_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
TEST_STREAM_DEPENDENCIES := $(TEST_STREAM_OBJECTS:%.o=%.d)
TEST_STREAM_LDFLAGS := -L$(OUTPUT_DIR) -lbuxtehude

# tests (validate)
TEST_VALIDATE_TARGET := $(OUTPUT_DIR)/valid-test
TEST_VALIDATE_SOURCE := tests/valid-test.cpp
TEST_VALIDATE_OBJECTS := $(TEST_VALIDATE_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
TEST_VALIDATE_DEPENDENCIES := $(TEST_VALIDATE_OBJECTS:%.o=%.d)
TEST_VALIDATE_LDFLAGS := -L$(OUTPUT_DIR) -lbuxtehude

# tests (bux)
TEST_BUX_TARGET := $(OUTPUT_DIR)/bux-test
TEST_BUX_SOURCE := tests/bux-test.cpp
TEST_BUX_OBJECTS := $(TEST_BUX_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
TEST_BUX_DEPENDENCIES := $(TEST_BUX_OBJECTS:%.o=%.d)
TEST_BUX_LDFLAGS := -L$(OUTPUT_DIR) -lbuxtehude -lfmt

UNAME := $(shell uname)

ifeq (${UNAME},Linux)
	CXXFLAGS := ${CXXFLAGS} -fPIC
	LDFLAGS := $(LIBRARIES) -shared

	BUXTEHUDE_DYNAMIC_TARGET := $(OUTPUT_DIR)/libbuxtehude.so # Build library

else ifeq (${UNAME},Darwin)
	LDRPATH := /usr/local/lib
	LDFLAGS := $(LIBRARIES) -dynamiclib -rpath $(LDRPATH) -install_name @rpath/libbuxtehude.dylib

	BUXTEHUDE_DYNAMIC_TARGET := $(OUTPUT_DIR)/libbuxtehude.dylib  # Build library

	TEST_STREAM_LDFLAGS := -rpath $(LDPATH) $(TEST_STREAM_LDFLAGS)
	TEST_VALIDATE_LDFLAGS := -rpath $(LDPATH) $(TEST_VALIDATE_LDFLAGS)
	TEST_BUX_LDFLAGS := -rpath $(LDPATH) $(TEST_BUX_LDFLAGS)
else
	ERROR := $(error Unknown Platform: $(UNAME))
endif

.PHONY: library test install uninstall clean realclean

library: $(BUXTEHUDE_DYNAMIC_TARGET) $(BUXTEHUDE_STATIC_TARGET)

$(BUXTEHUDE_DYNAMIC_TARGET): $(BUXTEHUDE_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUXTEHUDE_STATIC_TARGET): $(BUXTEHUDE_OBJECTS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_STREAM_TARGET): $(TEST_STREAM_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_STREAM_LDFLAGS) $^ -o $@

$(TEST_VALIDATE_TARGET): $(TEST_VALIDATE_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_STREAM_LDFLAGS) $^ -o $@

$(TEST_BUX_TARGET): $(TEST_BUX_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_BUX_LDFLAGS) $^ -o $@

test: library $(TEST_STREAM_TARGET) $(TEST_VALIDATE_TARGET) $(TEST_BUX_TARGET)
	@echo "Running tests..."
	export LD_LIBRARY_PATH=$(OUTPUT_DIR) && \
		$(TEST_STREAM_TARGET) && $(TEST_VALIDATE_TARGET) && $(TEST_BUX_TARGET)

# Rudimentary install for now
install: $(BUXTEHUDE_DYNAMIC_TARGET)
	cp $(BUXTEHUDE_DYNAMIC_TARGET) /usr/local/lib/
	cp $(BUXTEHUDE_STATIC_TARGET) /usr/local/lib
	cp -r $(BUXTEHUDE_INCLUDE_DIR) /usr/local/include/buxtehude

uninstall:
	rm /usr/local/lib/$(notdir $(BUXTEHUDE_DYNAMIC_TARGET)) /usr/local/lib/$(notdir $(BUXTEHUDE_STATIC_TARGET))
	rm -r /usr/local/include/buxtehude

clean:
	rm -rf $(BUILD_DIR)

realclean: clean
	rm -rf $(OUTPUT_DIR)

-include $(BUXTEHUDE_DEPENDENCIES) $(TEST_STREAM_DEPENDENCIES)
