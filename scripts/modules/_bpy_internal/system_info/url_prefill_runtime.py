# SPDX-FileCopyrightText: 2019-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Keep the information collected in this script synchronized with `startup.py`.

__all__ = (
    "url_from_blender",
)


def url_from_blender(*, addon_info=None):
    import bpy
    import gpu
    import struct
    import platform
    import urllib.parse

    # Build system info for GitHub issue body (markdown format)
    os_info = "{:s} {:d} Bits".format(
        platform.platform(),
        struct.calcsize("P") * 8,
    )

    # Windowing Environment (include when dynamically selectable).
    # This lets us know if WAYLAND/X11 is in use.
    from _bpy import _ghost_backend
    ghost_backend = _ghost_backend()
    if ghost_backend not in {'NONE', 'DEFAULT'}:
        os_info += (", {:s} UI".format(ghost_backend))
    del _ghost_backend, ghost_backend

    gpu_info = "{:s} {:s} {:s}".format(
        gpu.platform.renderer_get(),
        gpu.platform.vendor_get(),
        gpu.platform.version_get(),
    )

    gpu_backend = gpu.platform.backend_type_get()
    if gpu_backend not in {'NONE', 'UNKNOWN', 'METAL'}:
        gpu_info += (" {:s} Backend".format(gpu_backend.title()))

    version_info = "{:s}, branch: {:s}, commit date: {:s} {:s}, hash: `{:s}`".format(
        bpy.app.version_string,
        bpy.app.build_branch.decode('utf-8', 'replace'),
        bpy.app.build_commit_date.decode('utf-8', 'replace'),
        bpy.app.build_commit_time.decode('utf-8', 'replace'),
        bpy.app.build_hash.decode('ascii'),
    )

    # Build GitHub issue body in markdown format
    body_lines = [
        "**Description of the bug:**",
        "",
        "",
        "**Steps to reproduce:**",
        "",
        "",
        "**Expected behavior:**",
        "",
        "",
        "---",
        "**System Information:**",
        "",
    ]

    if addon_info:
        body_lines.append("**Add-on:** " + addon_info)

    body_lines.extend([
        "- **OS:** " + os_info,
        "- **GPU:** " + gpu_info,
        "- **Blender Version:** " + version_info,
    ])

    body = "\n".join(body_lines)

    query_params = {
        "title": "Bug Report",
        "body": body,
    }

    query_str = urllib.parse.urlencode(query_params)
    return "https://github.com/NazirJonh/X-Nazir-Build/issues/new?" + query_str
