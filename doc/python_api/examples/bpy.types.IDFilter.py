"""
Custom ID Filter Example
++++++++++++++++++++++++

An ``IDFilter`` lets a script decide which data-blocks the ID-browser popover
(:class:`bpy.types.UILayout.template_ID_browser`) and the filtered ID search
(:class:`bpy.types.UILayout.template_ID_with_filter_context`) offer.

Subclass ``bpy.types.IDFilter``, implement the ``filter_id`` class-method and
register the class, then reference it by ``bl_idname`` via the ``filter_type``
argument. ``filter_id`` is called once per candidate data-block and returns
``True`` to keep it.

The naming convention follows the other registered UI types (panels, menus,
UI-lists): an ``_IDF_`` infix is required in ``bl_idname``.

``template_ID_browser`` can be used from any editor (its popover state lives on
the window manager); the paint-slot/material filters are shown only when an Image
or Node editor is active, but the search, grid/list toggle and a custom
``filter_type`` work everywhere.

.. note::

   IDFilter subclasses must be registered for Blender to use them. ``filter_id``
   runs during the UI draw, so keep it cheap and side-effect free.
"""
import bpy


class IMAGE_IDF_tagged(bpy.types.IDFilter):
    # Only show images whose name starts with "T_" (a common texture prefix).
    # `context` is the current context, `id` is the candidate data-block
    # (already refined to its concrete type, e.g. an `Image`).
    @classmethod
    def filter_id(cls, context, id):
        return id.name.startswith("T_")


# A small panel that browses the active image with the filter applied.
class IMAGE_PT_idfilter_example(bpy.types.Panel):
    """Creates a Panel in the Image editor's tool region"""
    bl_label = "ID Filter Example"
    bl_idname = "IMAGE_PT_idfilter_example"
    bl_space_type = 'IMAGE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Image"

    def draw(self, context):
        layout = self.layout
        space = context.space_data

        # The browsed data-blocks come from the pointer property's ID type
        # (here `Image`). `filter_type` narrows them with the registered filter.
        layout.template_ID_browser(
            space, "image",
            new="image.new", open="image.open",
            filter_type="IMAGE_IDF_tagged",
        )

        # The same filter also works with the regular (search-menu) ID browser.
        layout.template_ID_with_filter_context(
            space, "image",
            new="image.new", open="image.open",
            filter_type="IMAGE_IDF_tagged",
        )


classes = (
    IMAGE_IDF_tagged,
    IMAGE_PT_idfilter_example,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
