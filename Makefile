# Returns all c files nested or not in $(1)
define collect_sources
	$(shell find $(1) -name '*.c')
endef

# Modify these variables to apply your preferences
OBJ_DIR := objects
EXE_NAME := bin

SOURCES := $(call collect_sources, src)
OBJECTS := $(patsubst %.c, $(OBJ_DIR)/%.o, $(SOURCES))

L_FLAGS := `pkg-config --libs x11`

.PHONY: start_server clean
.ALL: start_server

# Xephyr starts a brand new X server (display :1) and redirects all visual
# output to an X window running on our current X server!
# Each server is responsible for a single display (which is a combination
#  of potentially multiple screens along with their input devices)
start_server: $(EXE_NAME)
	# Using :100 to avoid any conflicts
	xinit ./xinitrc -- /usr/bin/Xephyr :100 -screen 800x600

$(EXE_NAME): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(EXE_NAME) $(L_FLAGS)

objects/%.o: %.c src/config.h
	@# Making sure that the directory already exists before creating the object
	@# All object files will be placed on a special, isolated directory
	@mkdir -p $(dir $@)

	$(CC) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)
	rm $(EXE_NAME)
