# ================================================================
#  OS Text Browser — Makefile
#  Usage:
#    make          → build the browser
#    make clean    → remove the binary and generated files
#    make run      → build + run
# ================================================================

CC      = gcc
TARGET  = browser
SRC     = browser.c

# GTK-3 flags (includes + libs)
GTK_FLAGS = $(shell pkg-config --cflags --libs gtk+-3.0)

CFLAGS  = -Wall -Wextra -Wno-deprecated-declarations -g
LIBS    = -lpthread -lrt

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(GTK_FLAGS) $(LIBS)
	@echo ""
	@echo "  Build successful!  Run with:  ./browser"
	@echo ""

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
	rm -f CacheMap.txt History.txt
	rm -rf Cache/
	@echo "Cleaned."

.PHONY: all run clean
