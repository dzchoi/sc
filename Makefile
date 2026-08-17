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
SRC_CPP = panel.cpp canvas.cpp
OBJ     = $(SRC_C:%.c=$(BUILDDIR)/%.o) $(SRC_CPP:%.cpp=$(BUILDDIR)/%.o)

CC      ?= gcc
CXX     ?= g++
CFLAGS  = -O2
STRIP   ?= strip
STCXXFLAGS = $(STCFLAGS) -std=c++17 -fno-exceptions -fno-rtti

all: $(BUILDDIR)/$(BIN) $(BUILDDIR)/$(CTL)

config.h:
	cp config.def.h config.h

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(STCFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(STCXXFLAGS) -c $< -o $@

$(BUILDDIR)/canvas.o:  canvas.cpp sc_config.hpp win.h
$(BUILDDIR)/st.o:      config.h st.h win.h panel.h
$(BUILDDIR)/x.o:       arg.h config.h st.h win.h panel.h
$(BUILDDIR)/panel.o:   canvas.hpp panel.h panel.hpp sc_config.hpp st.h

$(OBJ): config.h config.mk

$(BUILDDIR)/$(BIN): $(OBJ)
	$(CXX) -o $@ $(OBJ) $(STLDFLAGS)
	$(STRIP) $(BUILDDIR)/$(BIN)

$(BUILDDIR)/$(CTL): scctl.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) $(BUILDDIR)/$(CTL)

clean:
	rm -rf $(BUILDDIR) st-$(VERSION).tar.gz

dist: clean
	mkdir -p st-$(VERSION)
	cp -R FAQ LEGACY TODO LICENSE Makefile README config.mk\
		config.def.h st.info st.1 arg.h st.h win.h panel.h panel.hpp canvas.hpp sc.zsh\
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
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < st.1 > $(DESTDIR)$(MANPREFIX)/man1/st.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/st.1
	mkdir -p $(DESTDIR)$(PREFIX)/share/sc
	cp -f sc.zsh $(DESTDIR)$(PREFIX)/share/sc/sc.zsh
	chmod 644 $(DESTDIR)$(PREFIX)/share/sc/sc.zsh
	tic -sx st.info
	@echo Please see the README file regarding the terminfo entry of st.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(CTL)
	rm -f $(DESTDIR)$(PREFIX)/share/sc/sc.zsh
	rm -f $(DESTDIR)$(MANPREFIX)/man1/st.1

.PHONY: all clean dist install uninstall
