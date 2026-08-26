# SC - Simple Commander
# See LICENSE file for copyright and license details.
.POSIX:
.SUFFIXES: .c .cpp .o

include config.mk

# C sources built with $(CC); the panel is C++ built with $(CXX). We link with $(CXX) so
# libstdc++ is pulled in automatically.
BUILDDIR = .build
BIN		= sc
CTL		= scctl
SRC_C   = st.c x.c
SRC_CPP = comm.cpp panel.cpp canvas.cpp shell.cpp
OBJ     = $(SRC_C:%.c=$(BUILDDIR)/%.o) $(SRC_CPP:%.cpp=$(BUILDDIR)/%.o)

CC      ?= gcc
CXX     ?= g++
CFLAGS  = -O2
STRIP   ?= strip
STCXXFLAGS = $(STCFLAGS) -std=c++17 -fno-exceptions -fno-rtti

all: $(BUILDDIR)/$(BIN) $(BUILDDIR)/$(CTL) $(BUILDDIR)/sc.zsh

config.h:
	cp config.def.h config.h

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(STCFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(STCXXFLAGS) -c $< -o $@

$(BUILDDIR)/canvas.o:  canvas.cpp canvas.hpp sc_config.hpp st.h win.h
$(BUILDDIR)/st.o:      comm_api.h config.h st.h win.h
$(BUILDDIR)/x.o:       arg.h comm_api.h config.h st.h win.h
$(BUILDDIR)/comm.o:    canvas.hpp comm.hpp comm_api.h panel.hpp sc_config.hpp shell.hpp st.h
$(BUILDDIR)/panel.o:   canvas.hpp comm.hpp panel.hpp sc_config.hpp shell.hpp st.h
$(BUILDDIR)/shell.o:   canvas.hpp comm.hpp panel.hpp sc_config.hpp shell.hpp st.h

$(OBJ): config.h config.mk

$(BUILDDIR)/$(BIN): $(OBJ)
	$(CXX) -o $@ $(OBJ) $(STLDFLAGS)
	$(STRIP) $(BUILDDIR)/$(BIN)

$(BUILDDIR)/$(CTL): scctl.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) $(BUILDDIR)/$(CTL)

# sc.zsh must sit beside the sc binary; Shell::setup_zsh_environment() resolves it as
# <exe_dir>/sc.zsh at runtime.
$(BUILDDIR)/sc.zsh: sc.zsh | $(BUILDDIR)
	ln -sf ../sc.zsh $@

clean:
	rm -rf $(BUILDDIR) st-$(VERSION).tar.gz

dist: clean
	mkdir -p st-$(VERSION)
	cp -R FAQ LEGACY TODO LICENSE Makefile README config.mk\
		config.def.h st.info st.1 arg.h st.h win.h comm_api.h panel.hpp comm.hpp shell.hpp canvas.hpp sc_config.hpp sc.zsh\
		$(SRC_C) $(SRC_CPP) scctl.c\
		st-$(VERSION)
	tar -cf - st-$(VERSION) | gzip > st-$(VERSION).tar.gz
	rm -rf st-$(VERSION)

install: $(BUILDDIR)/$(BIN) $(BUILDDIR)/$(CTL)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(BUILDDIR)/$(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f $(BUILDDIR)/$(CTL) $(DESTDIR)$(PREFIX)/bin/$(CTL)
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(CTL)
	cp -f sc.zsh $(DESTDIR)$(PREFIX)/bin/sc.zsh
	chmod 644 $(DESTDIR)$(PREFIX)/bin/sc.zsh
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < st.1 > $(DESTDIR)$(MANPREFIX)/man1/st.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/st.1
	tic -sx st.info
	@echo Please see the README file regarding the terminfo entry of st.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(CTL)
	rm -f $(DESTDIR)$(PREFIX)/bin/sc.zsh
	rm -f $(DESTDIR)$(MANPREFIX)/man1/st.1

.PHONY: all clean dist install uninstall
