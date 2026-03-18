"""Tiny example controller script for pyemu.

This uses logical action names instead of Game Boy-specific masks so the same
style can carry across future cores.
"""


def on_reset(api):
    pass


def on_frame(api):
    frame = api.frame_index
    actions = ["down"]
    if frame % 45 == 0:
        actions.append("a")
    return {"actions": actions}
