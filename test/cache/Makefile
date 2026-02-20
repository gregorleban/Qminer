GCC=g++

ROOT=/Users/blazf/Projects/qminer/
QMINER_BUILD=Debug

COMPILE_FLAGS= -std=c++11 -fPIC -Wall -Wno-unknown-pragmas -Wsign-compare -Wpointer-arith -Wno-deprecated-declarations -Wno-cast-align

LINK_FLAGS= -lm -lz -ldl -lpthread
INCLUDES= -I$(ROOT)/src/glib/base -I$(ROOT)/src/glib/mine -I$(ROOT)/src/qminer
LINK_INCLUDES=${ROOT}/build/$(QMINER_BUILD)/glib.a ${ROOT}/build/$(QMINER_BUILD)/qminer.a

release: COMPILE_FLAGS += -DNDEBUG -g -O3

debug: COMPILE_FLAGS += -DDEBUG -O0 -g -ggdb
debug: LINK_FLAGS += -rdynamic

all: test_cache.cpp
	$(GCC) $(COMPILE_FLAGS) $(INCLUDES) -c test_cache.cpp -o test_cache.o
	$(GCC) $(COMPILE_FLAGS) $(INCLUDES) test_cache.o $(LINK_INCLUDES) -o test_cache $(LINK_FLAGS)

clean:
	rm -f test_cache.o, test_cache
