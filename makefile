# CC = m68k-atari-mint-gcc
CC = m68k-atari-mint-g++

STRIP = m68k-atari-mint-strip
STACK_BIN = m68k-atari-mint-stack
STACK_SIZE = 256k

SRC_DIR := ./
OBJ_DIR := ./build
BIN_DIR := ./bin


SRC := $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/*/*.cpp)

BIN := $(BIN_DIR)/mm_mint_ffmpeg.prg

OBJ := $(SRC:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

CPPFLAGS := -I./ 

CFLAGS   := -m68020 -m68881 -fomit-frame-pointer -O3 -ffast-math

LDFLAGS  :=

LDLIBS   := -lgem -lpthread -lpng16 -lz -lm -lavformat -lavcodec -lavutil -lswscale -lswresample -lfribidi -llcms2 -lxml2 -liconv -lssl -lcrypto -lfreetype -lbz2 -lpng16 -lm -lz -lpthread -lwebp -lvpx -llzma -lx264 -lx265 -lstdc++ -ltheora -lopus -lwebpdemux -lwebpmux -lwebpdecoder -lvorbisenc -lvorbis -logg -lmp3lame -laacplus -laom -lfdk-aac

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@
	$(STRIP) -s $(BIN)
	$(STACK_BIN) --fix=$(STACK_SIZE) $(BIN)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
	
$(BIN_DIR) $(OBJ_DIR):
	@mkdir -p $(@D)

clean:
	@$(RM) -rv $(BIN) $(OBJ_DIR)

-include $(OBJ:.o=.d)