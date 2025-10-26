CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2
LDFLAGS = -lncurses

TARGET = slurmtop
SOURCES = slurmtop.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	@echo "Building slurmtop..."
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES) $(LDFLAGS)
	@echo "Build successful! Run with: ./slurmtop [username]"
	@echo ""
	@echo "Controls:"
	@echo "  1-4: Switch views (Overview/Running/Pending/All)"
	@echo "  ↑/↓: Scroll up/down"
	@echo "  PgUp/PgDn: Scroll by page"
	@echo "  R: Refresh"
	@echo "  Q: Quit"

clean:
	rm -f $(TARGET)

.PHONY: all clean
