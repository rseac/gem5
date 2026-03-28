
include ../makefiles/Makefile.defs

COMMON_FILES=dummy.o common.o

BIN_DIR=../bin/$(COMPILER)

libs=-lm $(m5ops_libs)

BINS = $(BIN_DIR)/tsvc_vec $(BIN_DIR)/tsvc_novec $(BIN_DIR)/tsvc_noScalvec
DUMPS = $(BINS:=.dump)

all : $(BINS) $(DUMPS)

$(BIN_DIR)/tsvc_% : tsvc_%.o $(COMMON_FILES)
	@mkdir -p $(BIN_DIR)
	$(CC) $^ $(libs) -o $@$(SUFFIX)

$(BIN_DIR)/tsvc_%.dump : $(BIN_DIR)/tsvc_%
	$(OBJDUMP) -d $< > $@

%_novec.o:%.c .FORCE
	$(CC) $(flags) $(novecflags) -c -o $@ $<

%_noScalvec.o:%.c .FORCE
	$(CC) $(flags) $(noscalvecflags) -c -o $@ $<

%_vec.o:%.c .FORCE
	$(CC) $(flags) $(vecflags) -c -o $@ $<

# Compiled with IEEE math flags to make initialisation/checksum calculations more reproducable
# Be wary that PGI doesn't respect IEEE properly with vectorisation
%.o:%.c .FORCE
	$(CC) $(flags) $(vecflags) $(ieee_math_flags) -c -o $@ $<

.PHONY: .FORCE clean
.FORCE:
	@true

clean :
	rm -f *.o *.s $(DUMPS)

