####################################################################
#
#  Name:         Makefile
#  Created by:   Stefan Ritt
#
#  Contents:     Makefile for the v1740 frontend
#
#  $Id: Makefile 3655 2007-03-21 20:51:28Z amaudruz $
#
#####################################################################
PATH := /home/deap/packages/newgcc/bin:$(PATH)
#
# If not connected to hardware, use this to simulate it and generate
# random data
SIMULATION=0
# Path to gcc 4.8.1 binaries (needed to use new C++ stuff)
PATH := /home/deap/packages/newgcc/bin:$(PATH)

#--------------------------------------------------------------------
# The MIDASSYS should be defined prior the use of this Makefile
ifndef MIDASSYS
missmidas::
	@echo "...";
	@echo "Missing definition of environment variable 'MIDASSYS' !";
	@echo "...";
endif

#--------------------------------------------------------------------
# The following lines contain specific switches for different UNIX
# systems. Find the one which matches your OS and outcomment the 
# lines below.
#
# get OS type from shell
OSTYPE = $(shell uname)

#-----------------------------------------
# This is for Linux
ifeq ($(OSTYPE),Linux)
OSTYPE = linux
endif

ifeq ($(OSTYPE),linux)
#OS_DIR = linux-m64
OS_DIR = linux
OSFLAGS = -DOS_LINUX -DLINUX
CFLAGS = -g -Wall -DSIMULATION=$(SIMULATION) #-fno-omit-frame-pointer 
LDFLAGS = -g -lm -lz -lutil -lnsl -lpthread -lrt -lc 
endif
#
ifeq ($(OSTYPE),Darwin)
OSTYPE = darwin
OS_DIR = darwin
endif

#-----------------------------------------
# optimize?

# CFLAGS += -O2

#-----------------------------------------
# ROOT flags and libs
#
#ifdef ROOTSYS
#ROOTCFLAGS := $(shell  $(ROOTSYS)/bin/root-config --cflags)
#ROOTCFLAGS += -DHAVE_ROOT -DUSE_ROOT
#ROOTLIBS   := $(shell  $(ROOTSYS)/bin/root-config --libs) -Wl,-rpath,$(ROOTSYS)/lib
#ROOTLIBS   += -lThread
#else
#missroot:
#	@echo "...";
#	@echo "Missing definition of environment variable 'ROOTSYS' !";
#	@echo "...";
#endif
#-------------------------------------------------------------------
# The following lines define directories. Adjust if necessary
#
MIDAS_INC    = $(MIDASSYS)/include
MIDAS_LIB    = $(MIDASSYS)/$(OS_DIR)/lib
MIDAS_SRC    = $(MIDASSYS)/src

####################################################################
# Lines below here should not be edited
####################################################################
#
# compiler
CC   = gcc # -std=c99
#CXX  = g++ -std=c++11
CXX  = g++ -std=c++0x
#
# MIDAS library
LIBMIDAS=-L$(MIDAS_LIB) -lmidas
#
# All includes
INCS = -I. -I./include -I$(MIDAS_INC) 

####################################################################
# General commands
####################################################################

all: fe
	@echo "***** Finished"
	@echo "***** Use 'make doc' to build documentation"

fe : feBuilder.exe


####################################################################
# Libraries/shared stuff
####################################################################

####################################################################
# Single-thread frontend
####################################################################

feBuilder.exe: $(LIB) $(MIDAS_LIB)/mfe.o feBuilder.o ebFragment.o ebFilterDecision.o ebSmartQTFilter.o
	$(CXX) $(OSFLAGS) feBuilder.o ebFragment.o ebSmartQTFilter.o ebFilterDecision.o $(MIDAS_LIB)/mfe.o $(LIB) $(LIBMIDAS) -o $@ $(LDFLAGS)

feBuilder.o : feBuilder.cxx ebSmartQTFilter.o
	$(CXX) $(CFLAGS) $(OSFLAGS) ebSmartQTFilter.o $(INCS) -c $< -o $@

ebFragment.o : ebFragment.cxx ebSmartQTFilter.o
	$(CXX) $(CFLAGS) $(OSFLAGS) ebSmartQTFilter.o $(INCS) -c $< -o $@

ebFilterDecision.o : ebFilterDecision.cxx
	$(CXX) $(CFLAGS) $(OSFLAGS) $(INCS) -c $< -o $@

ebSmartQTFilter.o : ebSmartQTFilter.cxx
	$(CXX) $(CFLAGS) $(OSFLAGS) $(INCS) -c $< -o $@

$(MIDAS_LIB)/mfe.o:
	@cd $(MIDASSYS) && make
####################################################################
# Clean
####################################################################

clean::
	rm -f *.o *.exe
	rm -f *~
	rm -rf html

#end file
