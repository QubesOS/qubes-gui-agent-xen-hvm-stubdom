PYTHON ?= python3
QUBES_GUI_AGENT_QEMU := $(dir $(lastword $(MAKEFILE_LIST)))

$(QUBES_GUI_AGENT_QEMU)/qubes-keycode2scancode.c: $(QUBES_GUI_AGENT_QEMU)/gen-keycode2scancode
	$(PYTHON) $< qubes_keycode2scancode > $@
