BUILD_DIR := ./build
BUXTEHUDE_INCLUDE_DIR := include
INCLUDE_DIRS := $(BUXTEHUDE_INCLUDE_DIR)
INCLUDE := $(addprefix -I,$(INCLUDE_DIRS))
LIBRARIES := -lfmt -levent_core -levent_pthreads
CXXFLAGS := -Wall -std=c++20
CPPFLAGS := $(INCLUDE) -MMD -MP
LDRPATH := /usr/local/lib
LDFLAGS := $(LIBRARIES) -dynamiclib -rpath $(LDRPATH) -install_name @rpath/libbuxtehude.dylib

# Build library

BUXTEHUDE_DYLIB_TARGET := libbuxtehude.dylib
BUXTEHUDE_STATIC_TARGET := libbuxtehude.a
BUXTEHUDE_SOURCE := $(wildcard src/*.cpp)
BUXTEHUDE_OBJECTS := $(BUXTEHUDE_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
BUXTEHUDE_DEPENDENCIES := $(BUXTEHUDE_OBJECTS:%.o=%.d)

library: $(BUXTEHUDE_DYLIB_TARGET) $(BUXTEHUDE_STATIC_TARGET)

$(BUXTEHUDE_DYLIB_TARGET): $(BUXTEHUDE_OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUXTEHUDE_STATIC_TARGET): $(BUXTEHUDE_OBJECTS)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Build tests

TEST_VALIDATE_TARGET := valid-test
TEST_VALIDATE_SOURCE := tests/valid-test.cpp
TEST_VALIDATE_OBJECTS := $(TEST_VALIDATE_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
TEST_VALIDATE_DEPENDENCIES := $(TEST_VALIDATE_OBJECTS:%.o=%.d)
TEST_VALIDATE_LDFLAGS := -rpath $(LDRPATH) -lbuxtehude

$(TEST_VALIDATE_TARGET): $(TEST_VALIDATE_OBJECTS)
	$(CXX) $(TEST_VALIDATE_LDFLAGS) $^ -o $@

TEST_BUX_TARGET := bux-test
TEST_BUX_SOURCE := tests/bux-test.cpp
TEST_BUX_OBJECTS := $(TEST_BUX_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
TEST_BUX_DEPENDENCIES := $(TEST_BUX_OBJECTS:%.o=%.d)
TEST_BUX_LDFLAGS := -rpath $(LDRPATH) -lbuxtehude -lfmt

$(TEST_BUX_TARGET): $(TEST_BUX_OBJECTS)
	$(CXX) $(TEST_BUX_LDFLAGS) $^ -o $@

test: $(TEST_VALIDATE_TARGET) $(TEST_BUX_TARGET)
	@echo "Running tests..."
	./$(TEST_VALIDATE_TARGET) && ./$(TEST_BUX_TARGET)

# Rudimentary install for now

install: $(BUXTEHUDE_DYLIB_TARGET)
	cp $(BUXTEHUDE_DYLIB_TARGET) /usr/local/lib/
	cp $(BUXTEHUDE_STATIC_TARGET) /usr/local/lib
	cp -r $(BUXTEHUDE_INCLUDE_DIR) /usr/local/include/buxtehude

uninstall:
	rm /usr/local/lib/$(BUXTEHUDE_DYLIB_TARGET) /usr/local/lib/$(BUXTEHUDE_STATIC_TARGET)
	rm -r /usr/local/include/buxtehude

.PHONY: clean
clean:
	rm -r $(BUILD_DIR)

-include $(BUXTEHUDE_DEPENDENCIES) $(TEST_STREAM_DEPENDENCIES)
