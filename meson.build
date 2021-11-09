qubes_gui_agent_ss = ss.source_set()

qubes_gui_agent_ss.add(vchan_xen, xen, files(
  'gui-common/double-buffer.c',
  'gui-common/txrx-vchan.c',
  'gui-agent-qemu/qubes-gui.c',
))

ui_modules += {'qubes-gui': qubes_gui_agent_ss}
