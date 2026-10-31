#
# Makefile for the unit tests using Catch2 v3
#
# Build variants (selected with BUILD=...):
#   default   - plain debug build
#   sanitize  - build with AddressSanitizer and UndefinedBehaviorSanitizer
#
# Every variant uses its own object directory (obj/<variant>).
#

# Path to the VDR include directory (the default fits a plugin located in
# vdr/PLUGINS/src/zapcache)
VDRINCDIR ?= ../../../../include

BUILD ?= default

CXX ?= g++
CXXFLAGS ?= -g -O1

override CXXFLAGS += -std=c++17 -W -Wall -Wextra -Werror=format -I.. -isystem $(VDRINCDIR) -D_GNU_SOURCE
override CXXFLAGS += $(shell pkg-config --cflags catch2-with-main)
LIBS = $(shell pkg-config --libs catch2-with-main)

ifeq ($(BUILD),sanitize)
SANITIZE_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
override CXXFLAGS += $(SANITIZE_FLAGS)
override LDFLAGS += $(SANITIZE_FLAGS)
else ifneq ($(BUILD),default)
$(error Unknown BUILD variant '$(BUILD)', use default or sanitize)
endif

TEST_SOURCES = $(sort $(wildcard test_*.cpp))
SOURCES_UNDER_TEST = zapbuffer.cpp

OBJDIR = obj/$(BUILD)
OBJS = $(addprefix $(OBJDIR)/, $(TEST_SOURCES:.cpp=.o) $(SOURCES_UNDER_TEST:.cpp=.o))
DEPS = $(OBJS:.o=.d)
TEST_EXEC = $(OBJDIR)/test_runner

vpath %.cpp . ..

.PHONY: all
all: $(TEST_EXEC)

$(TEST_EXEC): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	@mkdir -p $@

-include $(DEPS)

# Run the tests
.PHONY: test
test: $(TEST_EXEC)
	$(TEST_EXEC)

.PHONY: clean
clean:
	rm -rf obj
