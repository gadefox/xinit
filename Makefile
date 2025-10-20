-include config.mk

CFLAGS += -Wall -std=c99 -pedantic -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L

all: config.mk outdir xinit

config.mk:
	@if ! test -e config.mk; then printf "\033[31;1mERROR:\033[0m you have to run ./configure\n"; exit 1; fi

OBJ = out/util.o \
			out/xinit.o

$(OBJ):
	$(QUIET_CC)$(CC) $(CFLAGS) -c src/$(@F:.o=.c) -o $@

xinit: $(OBJ)
	$(QUIET_LINK)$(CC) $^ $(LIBS) -o out/$@

outdir:
	@mkdir -p out

install:
	@echo installing xinit
	@mkdir -p $(DESTDIR)$(BIN_DIR)
	@cp -f out/xinit $(DESTDIR)$(BIN_DIR)
	@strip -s $(DESTDIR)$(BIN_DIR)/xinit
	@chmod 755 $(DESTDIR)$(BIN_DIR)/xinit
	@echo installing manual
	@mkdir -p $(DESTDIR)$(MAN_DIR)
	@cp -f data/xinit.1 $(DESTDIR)$(MAN_DIR)
	@gzip -f -9 $(DESTDIR)$(MAN_DIR)/xinit.1
	@chmod 644 $(DESTDIR)$(MAN_DIR)/xinit.1.gz

uninstall:
	@echo uninstalling xinit
	@rm -f $(DESTDIR)$(BIN_DIR)/xinit
	@echo uninstalling manual
	@rm -f $(DESTDIR)$(MAN_DIR)/xinit.1.gz

clean:
	@echo removing xprop output files..
	@rm -f out/*.o
	@rm -f out/xinit

distclean: clean
	@echo removing config.mk include file
	@rm -f config.mk

.PHONY: all clean distclean install uninstall
