# 🛹 SHANKPIT Master Makefile (CI-safe, deterministic outputs)

# ---- Tooling ----
CC       := gcc
BIN_DIR  := bin

# ---- Flags ----
CFLAGS   := -O2 -Wall -D_REENTRANT
INCLUDES := -Ipackages/common -Ipackages/simulation -Ipackages/render

LIBS_GL  := -lSDL2 -lGL -lGLU -lm
LIBS_M   := -lm

# ---- Sources ----
LOBBY_SRC    := apps/lobby/src/main.c packages/render/proc_tex.c
SERVER_SRC   := apps/server/src/main.c
SERVERCTL_SRC:= apps/server/serverctl.c

# ---- Outputs ----
LOBBY_BIN    := $(BIN_DIR)/shank_lobby
SERVER_BIN   := $(BIN_DIR)/shank_server
SERVERCTL_BIN:= $(BIN_DIR)/serverctl

# ---- Targets ----
.PHONY: all lobby server serverctl clean setup print

all: $(LOBBY_BIN) $(SERVER_BIN)

# Ensure bin/ exists even when building a single target
$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

setup: $(BIN_DIR)

# ---- CLIENT / LOBBY ----
lobby: $(LOBBY_BIN)

$(LOBBY_BIN): $(LOBBY_SRC) | $(BIN_DIR)
	@echo "🔨 Building Lobby Client..."
	$(CC) $(CFLAGS) $(INCLUDES) $(LOBBY_SRC) -o $@ $(LIBS_GL)

# ---- GAME SERVER ----
server: $(SERVER_BIN)

$(SERVER_BIN): $(SERVER_SRC) | $(BIN_DIR)
	@echo "🔨 Building Game Server..."
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LIBS_M)

# ---- SERVER CONTROL (OPTIONAL, LOCAL ONLY) ----
serverctl: $(SERVERCTL_BIN)

$(SERVERCTL_BIN): $(SERVERCTL_SRC) | $(BIN_DIR)
	@echo "🖥️ Building Server Control (requires ncurses)..."
	$(CC) -O2 $< -o $@ -lncurses

clean:
	@echo "🧹 Cleaning..."
	rm -rf $(BIN_DIR)

print:
	@echo "LOBBY_BIN=$(LOBBY_BIN)"
	@echo "SERVER_BIN=$(SERVER_BIN)"
