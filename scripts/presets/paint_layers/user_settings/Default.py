import bpy

# Matches the C++ defaults: the starter set of BKE_paint_layers_starter_channel_set, the
# auto-activate switch enabled, and the cache budgets unset (0 keeps the built-in per-cache
# defaults; set MiB to bound their sum for heavy 4K stacks).
prefs = bpy.context.preferences.edit

prefs.paint_layers_auto_activate_tab = True
prefs.paint_layers_paint_default_channels = {'BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL'}
prefs.paint_layers_fill_default_channels = {'BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL'}
prefs.paint_layers_cache_budget_mb = 0
