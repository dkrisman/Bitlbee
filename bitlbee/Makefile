###########################
## Makefile for BitlBee  ##
##                       ##
## Copyright 2002 Lintux ##
###########################

### DEFINITIONS

-include Makefile.settings

# Program variables
objects = bitlbee.o dcc.o help.o ipc.o irc.o irc_im.o irc_channel.o irc_commands.o irc_send.o irc_user.o irc_util.o nick.o $(OTR_BI) query.o root_commands.o set.o storage.o $(STORAGE_OBJS)
headers = bitlbee.h commands.h conf.h config.h help.h ipc.h irc.h log.h nick.h query.h set.h sock.h storage.h lib/events.h lib/ftutil.h lib/http_client.h lib/ini.h lib/md5.h lib/misc.h lib/proxy.h lib/sha1.h lib/ssl_client.h lib/url.h protocols/account.h protocols/bee.h protocols/ft.h protocols/nogaim.h
subdirs = lib protocols

ifeq ($(TARGET),i586-mingw32msvc)
objects += win32.o
LFLAGS+=-lws2_32
EFLAGS+=-lsecur32
OUTFILE=bitlbee.exe
else
objects += unix.o conf.o log.o
OUTFILE=bitlbee
endif

# Expansion of variables
subdirobjs = $(foreach dir,$(subdirs),$(dir)/$(dir).o)

all: $(OUTFILE) $(OTR_PI)
	$(MAKE) -C doc

uninstall: uninstall-bin uninstall-doc 
	@echo -e '\nmake uninstall does not remove files in '$(DESTDIR)$(ETCDIR)', you can use make uninstall-etc to do that.\n'

install: install-bin install-doc install-plugins
	@if ! [ -d $(DESTDIR)$(CONFIG) ]; then echo -e '\nThe configuration directory $(DESTDIR)$(CONFIG) does not exist yet, don'\''t forget to create it!'; fi
	@if ! [ -e $(DESTDIR)$(ETCDIR)/bitlbee.conf ]; then echo -e '\nNo files are installed in '$(DESTDIR)$(ETCDIR)' by make install. Run make install-etc to do that.'; fi
	@echo

.PHONY:   install   install-bin   install-etc   install-doc install-plugins \
        uninstall uninstall-bin uninstall-etc uninstall-doc \
        all clean distclean tar $(subdirs)

Makefile.settings:
	@echo
	@echo Run ./configure to create Makefile.settings, then rerun make
	@echo

clean: $(subdirs)
	rm -f *.o $(OUTFILE) core utils/bitlbeed
	$(MAKE) -C tests clean

distclean: clean $(subdirs)
	rm -rf .depend
	rm -f Makefile.settings config.h bitlbee.pc
	find . -name 'DEADJOE' -o -name '*.orig' -o -name '*.rej' -o -name '*~' -exec rm -f {} \;
	@# May still be present in dirs of disabled protocols.
	-find . -name .depend -exec rm -rf {} \;
	$(MAKE) -C tests distclean

check: all
	$(MAKE) -C tests

gcov: check
	gcov *.c

lcov: check
	lcov --directory . --capture --output-file bitlbee.info
	genhtml -o coverage bitlbee.info

install-doc:
	$(MAKE) -C doc install

uninstall-doc:
	$(MAKE) -C doc uninstall

install-bin:
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 $(OUTFILE) $(DESTDIR)$(BINDIR)/$(OUTFILE)

uninstall-bin:
	rm -f $(DESTDIR)$(BINDIR)/$(OUTFILE)

install-dev:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	install -m 0644 config.h $(DESTDIR)$(INCLUDEDIR)
	for i in $(headers); do install -m 0644 $(SRCDIR)$$i $(DESTDIR)$(INCLUDEDIR); done
	mkdir -p $(DESTDIR)$(PCDIR)
	install -m 0644 bitlbee.pc $(DESTDIR)$(PCDIR)

uninstall-dev:
	rm -f $(foreach hdr,$(headers),$(DESTDIR)$(INCLUDEDIR)/$(hdr))
	-rmdir $(DESTDIR)$(INCLUDEDIR)
	rm -f $(DESTDIR)$(PCDIR)/bitlbee.pc

install-etc:
	mkdir -p $(DESTDIR)$(ETCDIR)
	install -m 0644 $(SRCDIR)motd.txt $(DESTDIR)$(ETCDIR)/motd.txt
	install -m 0644 $(SRCDIR)bitlbee.conf $(DESTDIR)$(ETCDIR)/bitlbee.conf

uninstall-etc:
	rm -f $(DESTDIR)$(ETCDIR)/motd.txt
	rm -f $(DESTDIR)$(ETCDIR)/bitlbee.conf
	-rmdir $(DESTDIR)$(ETCDIR)

install-plugins:
ifdef OTR_PI
	mkdir -p $(DESTDIR)$(PLUGINDIR)
	install -m 0755 otr.so $(DESTDIR)$(PLUGINDIR)
endif

tar:
	fakeroot debian/rules clean || make distclean
	x=$$(basename $$(pwd)); \
	cd ..; \
	tar czf $$x.tar.gz --exclude=debian --exclude=.bzr* --exclude=.depend $$x

$(subdirs):
	@$(MAKE) -C $@ $(MAKECMDGOALS)

$(OTR_PI): %.so: $(SRCDIR)%.c
	@echo '*' Building plugin $@
	@$(CC) $(CFLAGS) $(OTRFLAGS) -fPIC -shared $< -o $@

$(objects): %.o: $(SRCDIR)%.c
	@echo '*' Compiling $<
	@$(CC) -c $(CFLAGS) $< -o $@

$(objects): Makefile Makefile.settings config.h

$(OUTFILE): $(objects) $(subdirs)
	@echo '*' Linking $(OUTFILE)
	@$(CC) $(objects) $(subdirobjs) -o $(OUTFILE) $(LFLAGS) $(EFLAGS)
ifndef DEBUG
	@echo '*' Stripping $(OUTFILE)
	@-$(STRIP) $(OUTFILE)
endif

ctags: 
	ctags `find . -name "*.c"` `find . -name "*.h"`

# Using this as a bogus Make target to test if a GNU-compatible version of
# make is available.
helloworld:
	@echo Hello World

-include .depend/*.d
