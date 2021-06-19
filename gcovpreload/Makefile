LIBNAME=libgcovpreload

# Fallback to gcc when $CC is not in $PATH.
CC:=$(shell sh -c 'type $${CC%% *} >/dev/null 2>/dev/null && echo $(CC) || echo gcc')
CXX:=$(shell sh -c 'type $${CXX%% *} >/dev/null 2>/dev/null && echo $(CXX) || echo g++')

DYLIBSUFFIX=so
DYLIBNAME=$(LIBNAME).$(DYLIBSUFFIX)
DYLIB_MAKE_CMD=$(CC) -shared -fpic gcovpreload.c -o ${DYLIBNAME}

all:
	$(DYLIB_MAKE_CMD)
	sudo cp $(DYLIBNAME) /usr/lib
	sudo chmod 777 -R /usr/lib/$(DYLIBNAME)
	sudo cp lcov_cobertura.py ../
