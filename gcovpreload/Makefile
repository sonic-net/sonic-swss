LIBNAME=libgcovpreload

# Fallback to gcc when $CC is not in $PATH.
CC:=$(shell sh -c 'type $${CC%% *} >/dev/null 2>/dev/null && echo $(CC) || echo gcc')
CXX:=$(shell sh -c 'type $${CXX%% *} >/dev/null 2>/dev/null && echo $(CXX) || echo g++')

DYLIBSUFFIX=so
DYLIBNAME=$(LIBNAME).$(DYLIBSUFFIX)
DYLIB_MAKE_CMD=$(CC) -shared -fpic gcovpreload.cpp -o ${DYLIBNAME}

all:
	$(DYLIB_MAKE_CMD)
	sudo cp $(DYLIBNAME) /usr/lib
	sudo chmod 777 -R /usr/lib/$(DYLIBNAME)
	sudo cp lcov_cobertura.py ../

Makefile:
install-binPROGRAMS:
uninstall-binPROGRAMS:
clean-binPROGRAMS:
gcovpreload:
mostlyclean-compile:
distclean-compile:
.cpp.o:
.cpp.obj:
.cpp.lo:
install:
check-am:
check:
install:
install-exec:
install-data:
uninstall:
install-am:
installcheck:
install-strip:
mostlyclean-generic:
clean-generic:
distclean-generic:
maintainer-clean-generic:
clean:
clean-am:
distclean:
distclean-am:
dvi:
dvi-am:
html:
info:
install-data-am:
install-dvi:
install-exec-am:
install-html:
install-info:
install-man:
install-pdf:
install-ps:
installcheck-am:
maintainer-clean:
mostlyclean:
pdf:
ps:
uninstall-am:
.MAKE:
.PHONY:
.PRECIOUS:
.NOEXPORT: 
