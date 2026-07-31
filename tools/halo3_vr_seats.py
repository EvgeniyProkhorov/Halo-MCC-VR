# Halo 3 seat cameras -> VR landmarks, with a seat jumper panel.
#
# Run this from Blender's Text Editor (Run Script). It:
#   1. turns on the "VR Scene Inspection" add-on,
#   2. makes one VR landmark per "cam:<vehicle>:<seat>" camera,
#   3. adds a "Halo Seats" tab in the 3D view sidebar (press N) where one
#      click switches to that vehicle's collection AND puts the VR view at
#      that seat,
#   4. sets VR scale to real-life 1:1 and pins the viewpoint exactly to the
#      authored eye point (positional tracking off) so what you see in the
#      headset is precisely the point the mod will use.
#
# Re-run it any time; it rebuilds the landmarks from whatever cameras exist.
# Ctrl+Shift+Right / Ctrl+Shift+Left step to the next/previous seat.
import bpy

PREFIX = "cam:"
ADDON = "viewport_vr_preview"


def ensure_addon():
    try:
        if ADDON not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module=ADDON)
    except Exception as exc:                                  # noqa: BLE001
        print("Could not enable %s: %s" % (ADDON, exc))
    return hasattr(bpy.types.Scene, "vr_landmarks")


def seat_cameras():
    """Cameras in collection order, so the list reads 01..13 like the file."""
    order = {c.name: i for i, c in
             enumerate(bpy.context.scene.collection.children)}
    cams = [o for o in bpy.data.objects
            if o.type == 'CAMERA' and o.name.startswith(PREFIX)]

    def key(obj):
        colls = [c.name for c in obj.users_collection]
        return (order.get(colls[0], 999) if colls else 999, obj.name)

    return sorted(cams, key=key)


def seat_label(cam):
    colls = [c.name for c in cam.users_collection]
    vehicle = colls[0] if colls else "?"
    seat = cam.name.split(":")[-1]
    return "%s  -  %s" % (vehicle, seat)


def build_landmarks():
    scene = bpy.context.scene
    cams = seat_cameras()
    scene.vr_landmarks.clear()
    for cam in cams:
        lm = scene.vr_landmarks.add()
        lm.name = seat_label(cam)
        lm.type = 'OBJECT'          # position + yaw of the camera, upright
        lm.base_pose_object = cam
        lm.base_scale = 1.0         # 1:1 with real life (scene is in metres)
    scene.vr_landmarks_selected = 0
    scene.vr_landmarks_active = 0
    return len(cams)


def apply_vr_defaults():
    wm = bpy.context.window_manager
    settings = getattr(wm, "xr_session_settings", None)
    if settings is None:
        return
    settings.base_scale = 1.0
    # Off = your eye sits exactly on the authored point (you still look
    # around freely). Turn it on in this panel to lean and peek about.
    settings.use_positional_tracking = False
    settings.show_floor = False
    settings.clip_start = 0.01


def isolate_collection(name):
    for lc in bpy.context.view_layer.layer_collection.children:
        lc.exclude = lc.name != name
        if lc.name == name:
            lc.hide_viewport = False


def goto_index(context, index):
    scene = context.scene
    if not scene.vr_landmarks:
        return False
    index = max(0, min(index, len(scene.vr_landmarks) - 1))
    lm = scene.vr_landmarks[index]
    cam = lm.base_pose_object
    if cam is not None:
        colls = [c.name for c in cam.users_collection]
        if colls:
            isolate_collection(colls[0])
        # select the camera so it is ready to nudge with G
        for obj in context.view_layer.objects:
            obj.select_set(False)
        if cam.name in context.view_layer.objects:
            cam.select_set(True)
            context.view_layer.objects.active = cam
    scene.vr_landmarks_selected = index
    scene.vr_landmarks_active = index          # moves a live VR session too
    return True


class HALO_OT_goto_seat(bpy.types.Operator):
    bl_idname = "halo.goto_seat"
    bl_label = "Go to seat"
    bl_description = ("Show this vehicle only and put the VR viewpoint at "
                      "this seat")
    bl_options = {'REGISTER', 'UNDO'}

    index: bpy.props.IntProperty(default=0)

    def execute(self, context):
        if not goto_index(context, self.index):
            self.report({'WARNING'}, "No seat landmarks - run the script")
            return {'CANCELLED'}
        return {'FINISHED'}


class HALO_OT_step_seat(bpy.types.Operator):
    bl_idname = "halo.step_seat"
    bl_label = "Next/previous seat"
    bl_options = {'REGISTER', 'UNDO'}

    delta: bpy.props.IntProperty(default=1)

    def execute(self, context):
        scene = context.scene
        if not scene.vr_landmarks:
            return {'CANCELLED'}
        count = len(scene.vr_landmarks)
        goto_index(context, (scene.vr_landmarks_active + self.delta) % count)
        return {'FINISHED'}


class HALO_OT_rebuild_seats(bpy.types.Operator):
    bl_idname = "halo.rebuild_seats"
    bl_label = "Rebuild seat list"
    bl_description = "Re-read the cam: cameras and rebuild the landmarks"

    def execute(self, context):
        count = build_landmarks()
        apply_vr_defaults()
        self.report({'INFO'}, "%d seats" % count)
        return {'FINISHED'}


class HALO_PT_seats(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Halo Seats"
    bl_label = "Seats"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        settings = getattr(context.window_manager, "xr_session_settings", None)

        row = layout.row(align=True)
        row.operator("halo.step_seat", text="Prev", icon='TRIA_LEFT').delta = -1
        row.operator("halo.step_seat", text="Next",
                     icon='TRIA_RIGHT').delta = 1
        if settings is not None:
            layout.prop(settings, "use_positional_tracking",
                        text="Let me lean around (off = exact eye point)")
        layout.operator("halo.rebuild_seats", icon='FILE_REFRESH')
        layout.separator()

        if not scene.vr_landmarks:
            layout.label(text="No seats yet - press Rebuild", icon='ERROR')
            return
        active = scene.vr_landmarks_active
        col = layout.column(align=True)
        for i, lm in enumerate(scene.vr_landmarks):
            row = col.row(align=True)
            row.alert = (i == active)
            op = row.operator("halo.goto_seat", text=lm.name,
                              icon='CAMERA_DATA' if i == active else 'DOT')
            op.index = i


CLASSES = (HALO_OT_goto_seat, HALO_OT_step_seat, HALO_OT_rebuild_seats,
           HALO_PT_seats)
_keymaps = []


def unregister():
    for km, kmi in _keymaps:
        try:
            km.keymap_items.remove(kmi)
        except Exception:                                     # noqa: BLE001
            pass
    _keymaps.clear()
    for cls in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:                                     # noqa: BLE001
            pass


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    kc = bpy.context.window_manager.keyconfigs.addon
    if kc:
        km = kc.keymaps.new(name='3D View', space_type='VIEW_3D')
        for key, delta in (('RIGHT_ARROW', 1), ('LEFT_ARROW', -1)):
            kmi = km.keymap_items.new("halo.step_seat", key, 'PRESS',
                                      ctrl=True, shift=True)
            kmi.properties.delta = delta
            _keymaps.append((km, kmi))


if __name__ == "__main__":
    unregister()
    if not ensure_addon():
        print("VR Scene Inspection add-on unavailable - enable it in "
              "Preferences > Add-ons, then run this again.")
    else:
        register()
        n = build_landmarks()
        apply_vr_defaults()
        print("Halo Seats ready: %d seat landmarks. Press N in the 3D view "
              "and pick the 'Halo Seats' tab." % n)
