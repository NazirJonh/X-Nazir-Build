# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Preferences synchronization from official Blender installations."""

import bpy
from bpy.types import Operator, PropertyGroup
from bpy.props import BoolProperty, EnumProperty, StringProperty, CollectionProperty

from bl_operators.userpref import (
    PREFERENCES_OT_copy_settings,
    _disable_missing_addons,
    _userconfig_path_is_default,
)


# Kept alive because Blender does not own the strings returned by a dynamic enum callback.
_sync_settings_version_items = []


def _sync_settings_version_items_cb(_self, _context):
    items = _sync_settings_version_items
    items.clear()
    versions = PREFERENCES_OT_copy_settings.find_versions_for_sync('STOCK')
    for version in versions:
        identifier = "{:d}.{:d}".format(*version)
        items.append((identifier, identifier, PREFERENCES_OT_copy_settings.version_path(version, 'STOCK')))
    if not items:
        items.append(('NONE', "None Found", "No official Blender configuration directory was found"))
    return items


def _persist_sync_source_version(context, version):
    if not version or version == 'NONE':
        return
    filepaths = context.preferences.filepaths
    if filepaths.sync_source_version != version:
        filepaths.sync_source_version = version
    bpy.ops.wm.save_userpref()


def _ensure_sync_settings_source_version(context):
    """Keep the transient UI property aligned with the value stored in user preferences."""
    settings = context.window_manager.sync_settings
    versions = PREFERENCES_OT_copy_settings.find_versions_for_sync('STOCK')
    identifiers = ["{:d}.{:d}".format(*version) for version in versions]
    saved = context.preferences.filepaths.sync_source_version.strip()
    if saved in identifiers:
        if settings.source_version != saved:
            settings.source_version = saved
        return
    if identifiers and settings.source_version not in identifiers:
        settings.source_version = identifiers[0]
    if identifiers and not saved and settings.source_version in identifiers:
        _persist_sync_source_version(context, settings.source_version)


_BLENDER_MANIFEST_FILENAME = "blender_manifest.toml"


def _load_blender_manifest(pkg_dir):
    import os

    manifest_path = os.path.join(pkg_dir, _BLENDER_MANIFEST_FILENAME)
    if not os.path.isfile(manifest_path):
        return None
    try:
        import tomllib
        with open(manifest_path, "rb") as manifest_file:
            return tomllib.load(manifest_file)
    except Exception:
        return None


def _extension_module_names(extensions_dir, *, exclude_theme_extensions=False):
    import os

    names = set()
    if not os.path.isdir(extensions_dir):
        return names
    for repo in os.listdir(extensions_dir):
        repo_dir = os.path.join(extensions_dir, repo)
        if repo.startswith('.') or not os.path.isdir(repo_dir):
            continue
        for module in os.listdir(repo_dir):
            module_dir = os.path.join(repo_dir, module)
            if module.startswith('.') or not os.path.isdir(module_dir):
                continue
            if exclude_theme_extensions:
                manifest = _load_blender_manifest(module_dir)
                if manifest is not None and manifest.get("type") == "theme":
                    continue
            names.add("{:s}/{:s}".format(repo, module))
    return names


def _extension_theme_packages(version_root):
    """Map ``repo/pkg`` keys to repository id, package id and display name."""
    import os

    packages = {}
    extensions_dir = os.path.join(version_root, "extensions")
    if not os.path.isdir(extensions_dir):
        return packages
    for repo in os.listdir(extensions_dir):
        repo_dir = os.path.join(extensions_dir, repo)
        if repo.startswith('.') or not os.path.isdir(repo_dir):
            continue
        for pkg_id in os.listdir(repo_dir):
            pkg_dir = os.path.join(repo_dir, pkg_id)
            if pkg_id.startswith('.') or not os.path.isdir(pkg_dir):
                continue
            manifest = _load_blender_manifest(pkg_dir)
            if manifest is None or manifest.get("type") != "theme":
                continue
            key = "{:s}/{:s}".format(repo, pkg_id)
            display_name = manifest.get("name") or bpy.path.display_name(pkg_id, title_case=False)
            packages[key] = (repo, pkg_id, display_name)
    return packages


def _interface_theme_preset_dirs(version_root):
    """Directories that may contain interface theme XML for a Blender version root."""
    import os

    candidates = (
        os.path.join(version_root, "scripts", "presets", "interface_theme"),
        os.path.join(version_root, "config", "scripts", "presets", "interface_theme"),
    )
    return [path for path in candidates if os.path.isdir(path)]


def _theme_preset_labels_by_filename(directory):
    """Map theme preset display labels to XML file names in one directory."""
    import os

    labels = {}
    if not os.path.isdir(directory):
        return labels
    for filename in os.listdir(directory):
        if filename.startswith('.') or not filename.lower().endswith('.xml'):
            continue
        labels[bpy.path.display_name(filename, title_case=False)] = filename
    return labels


def _theme_preset_labels_from_directories(directories):
    import os

    labels = {}
    for directory in directories:
        for label, filename in _theme_preset_labels_by_filename(directory).items():
            labels.setdefault(label, (directory, filename))
    return labels


def _theme_preset_labels_current():
    return _theme_preset_labels_from_directories(bpy.utils.preset_paths("interface_theme"))


def _theme_preset_labels_source(source_root):
    return _theme_preset_labels_from_directories(_interface_theme_preset_dirs(source_root))


def _add_theme_preset_changes(settings, source_root, dest_root):
    """Compare local and extension theme presets between two Blender configuration directories."""
    source_themes = _theme_preset_labels_source(source_root)
    dest_themes = _theme_preset_labels_current()

    for label in sorted(set(source_themes) - set(dest_themes)):
        item = settings.changes.add()
        item.category, item.name, item.status = "Themes", label, 'ADD'
        item.selected = True

    for label in sorted(set(source_themes) & set(dest_themes)):
        item = settings.changes.add()
        item.category, item.name, item.status = "Themes", label, 'UPDATE'
        item.selected = True

    source_extension_themes = _extension_theme_packages(source_root)
    dest_extension_themes = _extension_theme_packages(dest_root)
    for key in sorted(set(source_extension_themes) - set(dest_extension_themes)):
        _repo, _pkg_id, display_name = source_extension_themes[key]
        item = settings.changes.add()
        item.category, item.name, item.status = "Themes", display_name, 'ADD'
        item.theme_is_extension = True
        item.theme_extension_id = key
        item.selected = True
    for key in sorted(set(source_extension_themes) & set(dest_extension_themes)):
        _repo, _pkg_id, display_name = source_extension_themes[key]
        item = settings.changes.add()
        item.category, item.name, item.status = "Themes", display_name, 'UPDATE'
        item.theme_is_extension = True
        item.theme_extension_id = key
        item.selected = True


def sync_change_extension_id(change):
    """Full extension key ``repo/pkg`` for sync and file copy."""
    if change.category != "Extensions":
        return change.name
    if change.extension_repo and change.extension_pkg:
        return "{:s}/{:s}".format(change.extension_repo, change.extension_pkg)
    return change.name


def sync_change_display_name(change):
    if change.category == "Themes" and change.theme_is_extension:
        return "{:s} (Extension)".format(change.name)
    if change.category == "Extensions" and change.extension_pkg:
        return change.extension_pkg
    return change.name


def _sync_settings_change_in_group(change, category, extension_repo=""):
    if change.category != category:
        return False
    if extension_repo and change.extension_repo != extension_repo:
        return False
    return True


def sync_settings_group_all_selected(settings, category, extension_repo=""):
    matched = [
        change for change in settings.changes
        if _sync_settings_change_in_group(change, category, extension_repo)
    ]
    if not matched:
        return False
    return all(change.selected for change in matched)


def sync_settings_draw_group_select(row, settings, category, extension_repo=""):
    """Checkbox in the Update column that selects or clears a group of changes."""
    all_selected = sync_settings_group_all_selected(settings, category, extension_repo)
    op = row.operator(
        "preferences.sync_settings_changes_select",
        text="",
        icon='CHECKBOX_HLT' if all_selected else 'CHECKBOX_DEHLT',
        emboss=False,
    )
    op.category = category
    op.extension_repo = extension_repo
    op.selected = not all_selected


class PREFERENCES_OT_sync_settings_changes_select(Operator):
    """Include or exclude a group of synchronization changes"""
    bl_idname = "preferences.sync_settings_changes_select"
    bl_label = "Select Group"
    bl_options = {'INTERNAL'}

    category: StringProperty()
    extension_repo: StringProperty(
        name="Repository",
        default="",
        description="Extension repository id; empty selects the whole category",
    )
    selected: BoolProperty(name="Selected")

    def execute(self, context):
        settings = context.window_manager.sync_settings
        for change in settings.changes:
            if _sync_settings_change_in_group(change, self.category, self.extension_repo):
                change.selected = self.selected
        return {'FINISHED'}


def _copy_selected_theme_presets(source_root, dest_root, settings):
    """Copy local theme XML presets and theme extension packages selected in the UI."""
    import os
    import shutil

    source_themes = _theme_preset_labels_source(source_root)
    dest_dir = bpy.utils.user_resource(
        'SCRIPTS',
        path=os.path.join("presets", "interface_theme"),
        create=True,
    )

    copied = 0
    for change in settings.changes:
        if change.category != 'Themes' or not change.selected:
            continue
        if change.status not in {'ADD', 'UPDATE'}:
            continue
        if change.theme_is_extension:
            if '/' not in change.theme_extension_id:
                continue
            repo, pkg_id = change.theme_extension_id.split('/', 1)
            source_pkg = os.path.join(source_root, "extensions", repo, pkg_id)
            dest_pkg = os.path.join(dest_root, "extensions", repo, pkg_id)
            if not os.path.isdir(source_pkg):
                continue
            if change.status == 'ADD' and os.path.exists(dest_pkg):
                continue
            os.makedirs(os.path.dirname(dest_pkg), exist_ok=True)
            shutil.copytree(source_pkg, dest_pkg, dirs_exist_ok=True, symlinks=True)
            copied += 1
            continue
        if not dest_dir:
            continue
        source_entry = source_themes.get(change.name)
        if source_entry is None:
            continue
        source_dir, filename = source_entry
        source_path = os.path.join(source_dir, filename)
        dest_path = os.path.join(dest_dir, filename)
        if change.status == 'ADD' and os.path.exists(dest_path):
            continue
        shutil.copy2(source_path, dest_path)
        copied += 1
    return copied


class PreferencesSyncChange(PropertyGroup):
    category: StringProperty()
    name: StringProperty()
    selected: BoolProperty(name="Update", description="Include this item in synchronization", default=True)
    status: EnumProperty(items=(
        ('ADD', "Add", ""),
        ('UPDATE', "Update", ""),
        ('DISABLE', "Disable", ""),
    ))
    theme_is_extension: BoolProperty(
        name="Extension Theme",
        default=False,
        options={'HIDDEN'},
    )
    theme_extension_id: StringProperty(
        name="Extension Theme ID",
        default="",
        options={'HIDDEN'},
        description="Repository and package id (repo/pkg) for theme extensions",
    )
    extension_repo: StringProperty(
        name="Extension Repository",
        default="",
        options={'HIDDEN'},
    )
    extension_pkg: StringProperty(
        name="Extension Package",
        default="",
        options={'HIDDEN'},
    )


def _sync_change_set_extension_item(item, category, status, extension_key):
    repo, _, pkg = extension_key.partition('/')
    item.category = category
    item.extension_repo = repo
    item.extension_pkg = pkg
    item.name = pkg
    item.status = status


class PreferencesSyncSettings(PropertyGroup):
    def _source_version_update(self, context):
        if self.source_version != 'NONE':
            _persist_sync_source_version(context, self.source_version)

    source_version: EnumProperty(
        name="Version",
        description="Official Blender version to take the settings from",
        items=_sync_settings_version_items_cb,
        update=_source_version_update,
    )
    use_addons: BoolProperty(name="Add-ons", default=True)
    use_repos: BoolProperty(name="Extension Repositories", default=True)
    use_files: BoolProperty(name="Extensions and Add-on Files", default=True)
    use_keymap: BoolProperty(name="Keymap", default=False)
    use_favorites: BoolProperty(name="Quick Favorites", default=False)
    use_paths: BoolProperty(name="Asset Libraries and Paths", default=True)
    use_theme: BoolProperty(name="Themes", default=False)
    show_addons: BoolProperty(name="Show Add-ons", default=True)
    show_extensions: BoolProperty(name="Show Extensions", default=True)
    show_asset_libraries: BoolProperty(name="Show Asset Libraries", default=True)
    show_themes: BoolProperty(name="Show Themes", default=True)
    changes: CollectionProperty(type=PreferencesSyncChange)


class PREFERENCES_OT_sync_settings_read(Operator):
    """Read and display the changes available from the selected Blender version"""
    bl_idname = "preferences.sync_settings_read"
    bl_label = "Read State"

    def execute(self, context):
        import os

        settings = context.window_manager.sync_settings
        settings.changes.clear()
        source = PREFERENCES_OT_copy_settings.version_path(
            tuple(int(value) for value in settings.source_version.split('.')),
            'STOCK') if settings.source_version != 'NONE' else None
        dest = bpy.utils.resource_path('USER')
        if not source or not os.path.isdir(source):
            self.report({'ERROR'}, "Official Blender configuration directory not found")
            return {'CANCELLED'}

        def add_changes(category, source_dir, dest_dir, *, nested=False, exclude_theme_extensions=False):
            if nested:
                source_names = _extension_module_names(
                    source_dir, exclude_theme_extensions=exclude_theme_extensions)
                dest_names = _extension_module_names(
                    dest_dir, exclude_theme_extensions=exclude_theme_extensions)
            else:
                source_names = set(os.listdir(source_dir)) if os.path.isdir(source_dir) else set()
                dest_names = set(os.listdir(dest_dir)) if os.path.isdir(dest_dir) else set()
            for name in sorted(source_names - dest_names):
                if not name.startswith('.'):
                    item = settings.changes.add()
                    if nested:
                        _sync_change_set_extension_item(item, category, 'ADD', name)
                    else:
                        item.category, item.name, item.status = category, name, 'ADD'
            for name in sorted(source_names & dest_names):
                if not name.startswith('.'):
                    item = settings.changes.add()
                    if nested:
                        _sync_change_set_extension_item(item, category, 'UPDATE', name)
                    else:
                        item.category, item.name, item.status = category, name, 'UPDATE'
            for name in sorted(dest_names - source_names):
                if not name.startswith('.'):
                    item = settings.changes.add()
                    if nested:
                        _sync_change_set_extension_item(item, category, 'DISABLE', name)
                    else:
                        item.category, item.name, item.status = category, name, 'DISABLE'

        if settings.use_files:
            add_changes(
                "Extensions",
                os.path.join(source, "extensions"),
                os.path.join(dest, "extensions"),
                nested=True,
                exclude_theme_extensions=True,
            )
            add_changes("Add-ons", os.path.join(source, "scripts", "addons"), os.path.join(dest, "scripts", "addons"))
        source_pref = os.path.join(source, "config", "userpref.blend")
        dest_pref = os.path.join(dest, "config", "userpref.blend")
        if settings.use_paths and os.path.isfile(source_pref) and os.path.isfile(dest_pref):
            result = bpy.ops.preferences.userdef_sync_analyze(filepath=source_pref)
            if result == {'FINISHED'}:
                analysis = context.window_manager.operator_properties_last(
                    "preferences.userdef_sync_analyze")
                for status, value in (("ADD", analysis.added),
                                      ("UPDATE", analysis.updated)):
                    for name in filter(None, value.split(';')):
                        item = settings.changes.add()
                        item.category, item.name, item.status = "Asset Libraries", name, status
                        item.selected = True
        elif settings.use_paths and os.path.isfile(source_pref):
            item = settings.changes.add()
            item.category, item.name, item.status = "Asset Libraries", "Configured paths", 'ADD'
            item.selected = True
        _add_theme_preset_changes(settings, source, dest)
        _persist_sync_source_version(context, settings.source_version)
        self.report({'INFO'}, "Read {:d} available change(s)".format(len(settings.changes)))
        return {'FINISHED'}


class PREFERENCES_OT_sync_settings_open(Operator):
    """Open the non-modal settings synchronization page"""
    bl_idname = "preferences.sync_settings_open"
    bl_label = "Sync from Official Blender"

    def execute(self, context):
        versions = PREFERENCES_OT_copy_settings.find_versions_for_sync('STOCK')
        settings = context.window_manager.sync_settings
        saved_version = context.preferences.filepaths.sync_source_version.strip()
        saved_version_exists = any(
            saved_version == "{:d}.{:d}".format(*version) for version in versions)
        if saved_version_exists:
            settings.source_version = saved_version
        else:
            settings.source_version = "{:d}.{:d}".format(*versions[0]) if versions else 'NONE'
        settings.changes.clear()
        context.preferences.active_section = 'SYNC_SETTINGS'
        return {'FINISHED'}


class PREFERENCES_OT_sync_settings(Operator):
    """Merge settings from an official Blender installation into this build"""
    bl_idname = "preferences.sync_settings"
    bl_label = "Sync Settings"

    source_version: EnumProperty(
        name="Version",
        description="Official Blender version to take the settings from",
        items=_sync_settings_version_items_cb,
    )
    use_addons: BoolProperty(
        name="Add-ons",
        description="Enabled add-ons and their preferences. Add-ons of this build that the source "
        "does not know about are kept",
        default=True,
    )
    use_repos: BoolProperty(
        name="Extension Repositories",
        description="Repositories that are not set up in this build yet. Existing ones are kept",
        default=True,
    )
    use_files: BoolProperty(
        name="Add-on Files",
        description="Copy extensions and legacy add-ons that are not installed in this build. "
        "Already installed ones are never overwritten",
        default=True,
    )
    use_theme: BoolProperty(
        name="Themes",
        description="Merge themes from the source preferences into this build",
        default=False,
    )
    use_keymap: BoolProperty(
        name="Keymap",
        description="Replace the key map and its preferences",
        default=False,
    )
    use_favorites: BoolProperty(
        name="Quick Favorites",
        description="Replace the quick favorites menu",
        default=False,
    )
    use_paths: BoolProperty(
        name="Paths",
        description="Asset libraries, script directories and auto-execution paths that are not "
        "set up in this build yet",
        default=False,
    )

    @classmethod
    def poll(cls, _context):
        return _userconfig_path_is_default()

    def _source_path(self):
        import os

        if self.source_version == 'NONE':
            return None
        major, minor = (int(number) for number in self.source_version.split("."))
        path = PREFERENCES_OT_copy_settings.version_path((major, minor), 'STOCK')
        return path if path and os.path.isdir(path) else None

    def _source_is_newer(self):
        if self.source_version == 'NONE':
            return False
        major, minor = (int(number) for number in self.source_version.split("."))
        return (major, minor) > bpy.app.version[:2]

    def invoke(self, context, _event):
        return self.execute(context)

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        layout.prop(self, "source_version")

        col = layout.column(heading="Merge")
        col.prop(self, "use_addons")
        col.prop(self, "use_repos")
        col.prop(self, "use_files")
        col.prop(self, "use_paths")
        col.prop(self, "use_theme")

        col = layout.column(heading="Replace")
        col.prop(self, "use_keymap")
        col.prop(self, "use_favorites")

        if self.use_keymap or self.use_favorites:
            box = layout.box()
            box.alert = True
            box.label(text="Replaced settings are overwritten entirely", icon='ERROR')
            box.label(text="A backup of the preferences file is created first")

        if self.use_theme:
            box = layout.box()
            box.label(text="Selected themes are merged by name", icon='INFO')

        if self._source_is_newer():
            box = layout.box()
            box.alert = True
            box.label(text="The source is newer than this build", icon='ERROR')
            box.label(text="Settings it does not share with this version are ignored")

    def execute(self, context):
        import os
        import shutil
        import time

        settings = context.window_manager.sync_settings
        self.source_version = settings.source_version
        self.use_addons = settings.use_addons
        self.use_repos = settings.use_repos
        self.use_files = settings.use_files
        self.use_theme = settings.use_theme
        self.use_keymap = settings.use_keymap
        self.use_favorites = settings.use_favorites
        self.use_paths = settings.use_paths

        source = self._source_path()
        if source is None:
            self.report({'ERROR'}, "Official Blender configuration directory not found")
            return {'CANCELLED'}

        dest = bpy.utils.resource_path('USER')
        if os.path.normcase(os.path.normpath(source)) == os.path.normcase(os.path.normpath(dest)):
            self.report({'ERROR'}, "Source and destination are the same directory")
            return {'CANCELLED'}

        source_userpref = os.path.join(source, "config", "userpref.blend")
        if not os.path.isfile(source_userpref):
            self.report({'ERROR'}, "No preferences file at \"{:s}\"".format(source_userpref))
            return {'CANCELLED'}

        # Preferences have no undo, so keep a copy of the file the merge is about to change.
        backup_dir = ""
        dest_userpref = os.path.join(dest, "config", "userpref.blend")
        if os.path.isfile(dest_userpref):
            backup_dir = os.path.join(dest, "backups", time.strftime("%Y-%m-%d_%H%M%S"))
            try:
                os.makedirs(backup_dir, exist_ok=True)
                shutil.copy2(dest_userpref, os.path.join(backup_dir, "userpref.blend"))
            except OSError as ex:
                self.report({'ERROR'}, "Unable to back up the preferences: {:s}".format(str(ex)))
                return {'CANCELLED'}

        copied = []
        themes_copied = 0
        if self.use_files:
            try:
                selected_addons = {
                    change.name for change in settings.changes
                    if change.selected and change.category == 'Add-ons'
                }
                selected_extensions = {
                    sync_change_extension_id(change) for change in settings.changes
                    if change.selected and change.category == 'Extensions'
                }
                copied = self._copy_missing_modules(
                    source,
                    dest,
                    selected_addons,
                    selected_extensions,
                    any(change.category == 'Add-ons' for change in settings.changes),
                    any(change.category == 'Extensions' for change in settings.changes),
                )
            except OSError as ex:
                self.report({'ERROR'}, "Unable to copy add-on files: {:s}".format(str(ex)))
                return {'CANCELLED'}

        if self.use_theme:
            try:
                themes_copied = _copy_selected_theme_presets(source, dest, settings)
            except OSError as ex:
                self.report({'ERROR'}, "Unable to copy theme presets: {:s}".format(str(ex)))
                return {'CANCELLED'}

        bpy.ops.preferences.userdef_merge(
            filepath=source_userpref,
            source_root=source,
            use_addons=self.use_addons,
            use_repos=self.use_repos,
            use_theme=self.use_theme,
            use_keymap=self.use_keymap,
            use_favorites=self.use_favorites,
            use_paths=self.use_paths,
            selected_addons=';'.join(
                change.name for change in settings.changes
                if change.selected and change.category == 'Add-ons'),
            selected_extensions=';'.join(
                sync_change_extension_id(change) for change in settings.changes
                if change.selected and change.category == 'Extensions'),
            selected_asset_libraries=';'.join(
                change.name for change in settings.changes
                if change.selected and change.category == 'Asset Libraries'),
            selected_themes=';'.join(
                change.name for change in settings.changes
                if change.selected and change.category == 'Themes'),
            selected_addons_active=any(change.category == 'Add-ons' for change in settings.changes),
            selected_extensions_active=any(change.category == 'Extensions' for change in settings.changes),
            selected_asset_libraries_active=any(
                change.category == 'Asset Libraries' for change in settings.changes),
            selected_themes_active=any(change.category == 'Themes' for change in settings.changes),
        )

        disabled = _disable_missing_addons(context)
        failed_enable = self._enable_merged_addons(context)
        _persist_sync_source_version(context, settings.source_version)

        message = "Synchronized settings from {:s}".format(source)
        if copied:
            message += ". Added {:d} add-on(s)".format(len(copied))
        if themes_copied:
            message += ". Added {:d} theme preset(s)".format(themes_copied)
        if backup_dir:
            message += ". Previous preferences copied to {:s}".format(backup_dir)
        if disabled:
            message += ". Disabled add-ons that are not installed: {:s}".format(", ".join(disabled))
        if failed_enable:
            message += ". Could not load add-on(s): {:s}".format(", ".join(failed_enable))
        self.report({'INFO'}, message)
        return {'FINISHED'}

    @staticmethod
    def _enable_merged_addons(context):
        """Enable add-ons the merge brought in that are not loaded yet.

        Without this, a merged add-on shows up enabled in the preferences but never actually
        registers, since merging only touches `UserDef.addons`, not the running Python session.

        :return: module names that could not be enabled.
        :rtype: list[str]
        """
        import addon_utils

        addon_utils.extensions_refresh()
        failed = []
        for addon in context.preferences.addons:
            module_name = addon.module
            _loaded_default, loaded_state = addon_utils.check(module_name)
            if loaded_state:
                continue
            if addon_utils.enable(
                    module_name,
                    default_set=True,
                    persistent=True,
                    refresh_handled=True,
            ) is None:
                failed.append(module_name)
        addon_utils.extensions_refresh()
        return failed

    @staticmethod
    def _copy_missing_modules(
        source, dest, selected_addons, selected_extensions, addons_selection_active,
        extensions_selection_active,
    ):
        """Copy extension and legacy add-on directories this build does not have yet.

        Never overwrites: an existing module may carry local fixes, and updating it is the job of
        the extension repository rather than of this operator.
        """
        import os
        import shutil

        copied = []

        # Extensions are grouped per repository: "extensions/<repo>/<module>".
        source_extensions = os.path.join(source, "extensions")
        if os.path.isdir(source_extensions):
            for repo in os.listdir(source_extensions):
                source_repo = os.path.join(source_extensions, repo)
                if repo.startswith(".") or not os.path.isdir(source_repo):
                    continue
                for module in os.listdir(source_repo):
                    source_module = os.path.join(source_repo, module)
                    if module.startswith(".") or not os.path.isdir(source_module):
                        continue
                    if (extensions_selection_active and
                            "{:s}/{:s}".format(repo, module) not in selected_extensions):
                        continue
                    dest_module = os.path.join(dest, "extensions", repo, module)
                    if os.path.exists(dest_module):
                        continue
                    os.makedirs(os.path.dirname(dest_module), exist_ok=True)
                    shutil.copytree(source_module, dest_module, symlinks=True)
                    copied.append("{:s}/{:s}".format(repo, module))

        # Legacy add-ons are flat: "scripts/addons/<module>", either a package or a single file.
        source_addons = os.path.join(source, "scripts", "addons")
        if os.path.isdir(source_addons):
            for module in os.listdir(source_addons):
                source_module = os.path.join(source_addons, module)
                is_package = os.path.isdir(source_module)
                if module.startswith(".") or not (is_package or module.endswith(".py")):
                    continue
                if (addons_selection_active and module.removesuffix(".py") not in selected_addons and
                        module not in selected_addons):
                    continue
                dest_module = os.path.join(dest, "scripts", "addons", module)
                if os.path.exists(dest_module):
                    continue
                os.makedirs(os.path.dirname(dest_module), exist_ok=True)
                if is_package:
                    shutil.copytree(source_module, dest_module, symlinks=True)
                else:
                    shutil.copy2(source_module, dest_module)
                copied.append(module)

        return copied

classes = (
    PreferencesSyncChange,
    PreferencesSyncSettings,
    PREFERENCES_OT_sync_settings_open,
    PREFERENCES_OT_sync_settings_read,
    PREFERENCES_OT_sync_settings_changes_select,
    PREFERENCES_OT_sync_settings,
)

