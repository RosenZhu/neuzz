#
# makefile
# -----------------------------
#
# Written by Xiaogang Zhu <xiaogangzhu@swin.edu.au>
#


##################################################################

# var- edit DYN_ROOT accordingly

DYN_ROOT 	= /apps/buildDyn101
# These should point to where libelf and libdwarf are installed
LOCAL_INC = /usr/local/include
LOCAL_LIBS = /usr/local/lib
# TBB_INC = $(DYN_ROOT)/tbb/include
DYNINST_INCLUDE = $(DYN_ROOT)/include
DYNINST_LIB =  $(DYN_ROOT)/lib

CC 			= gcc 
CXX 		= g++
CXXFLAGS 	= -g -Wall -O3 -std=c++11
LIBFLAGS 	= -fpic -shared
LDFLAGS 	= -I/usr/include -I$(DYNINST_INCLUDE) -I$(LOCAL_INC) -L$(DYNINST_LIB) -L$(LOCAL_LIBS)\
					-lcommon -liberty -ldyninstAPI -lboost_system -linstructionAPI -lstdc++fs


# PROGS intentionally omit as, which gets installed elsewhere.

PROGS       = neuzz libInstDyninst InstDyninst


CFLAGS     ?= -O3 -funroll-loops


ifneq "$(filter Linux GNU%,$(shell uname))" ""
  LDFLAGS  += -ldl
endif


all: $(PROGS) all_done


# Inst dependencies

neuzz: neuzz.c
	$(CC) $(CFLAGS) $@.c -o $@

libInstDyninst: libInstDyninst.cpp
	$(CXX) $(CXXFLAGS) -o libInstDyninst.so libInstDyninst.cpp $(LDFLAGS) $(LIBFLAGS)

InstDyninst: InstDyninst.cpp
	$(CXX) -Wl,-rpath-link,$(DYN_ROOT)/lib -Wl,-rpath-link,$(DYN_ROOT)/include $(CXXFLAGS) -o InstDyninst InstDyninst.cpp $(LDFLAGS)





all_done: 
	@echo "[+] All done!"
	
.NOTPARALLEL: clean

clean:
	rm -f $(PROGS) *.o  *.so


