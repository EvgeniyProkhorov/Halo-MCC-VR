# Halo: Reach vehicle-seat cameras -> VR landmarks and seat jumper.
#
# Run from Blender's Text Editor. Re-running is safe. Rebuild and seat jumping
# never move a camera; the Reach Seats panel exposes translation-only controls
# for the active eye point.
bl_info = {
    "name": "Reach Vehicle Seat Authoring",
    "author": "Halo MCC VR",
    "version": (1, 1, 0),
    "blender": (4, 3, 0),
    "category": "3D View",
}

import bpy


PREFIX = "cam:"
ADDON = "viewport_vr_preview"
WU_TO_M = 3.048


def ensure_addon():
    try:
        if ADDON not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module=ADDON)
    except Exception as exc:  # Blender installations can omit the VR add-on.
        print("Could not enable %s: %s" % (ADDON, exc))
    return hasattr(bpy.types.Scene, "vr_landmarks")


def seat_cameras():
    order = {
        collection.name: index
        for index, collection in
        enumerate(bpy.context.scene.collection.children)
    }
    cameras = [
        obj for obj in bpy.data.objects
        if obj.type == 'CAMERA' and obj.name.startswith(PREFIX)
    ]

    def key(obj):
        collections = [collection.name for collection in obj.users_collection]
        collection_order = order.get(collections[0], 999) if collections else 999
        return collection_order, obj.name

    return sorted(cameras, key=key)


def camera_parts(camera):
    parts = camera.name.split(":")
    if len(parts) == 3:
        return parts[1], parts[2]
    return "unknown", camera.name


def seat_label(camera):
    collections = [collection.name for collection in camera.users_collection]
    collection = collections[0] if collections else "?"
    vehicle, seat = camera_parts(camera)
    return "%s  -  %s:%s" % (collection, vehicle, seat)


def build_landmarks():
    scene = bpy.context.scene
    cameras = seat_cameras()
    scene.vr_landmarks.clear()
    for camera in cameras:
        landmark = scene.vr_landmarks.add()
        landmark.name = seat_label(camera)
        landmark.type = 'OBJECT'
        landmark.base_pose_object = camera
        landmark.base_scale = 1.0
    scene.vr_landmarks_selected = 0
    scene.vr_landmarks_active = 0
    return len(cameras)


def apply_vr_defaults():
    settings = getattr(
        bpy.context.window_manager, "xr_session_settings", None)
    if settings is None:
        return
    settings.base_scale = 1.0
    settings.use_positional_tracking = False
    settings.show_floor = False
    settings.clip_start = 0.01


def isolate_collection(name):
    for layer_collection in bpy.context.view_layer.layer_collection.children:
        layer_collection.exclude = layer_collection.name != name
        if layer_collection.name == name:
            layer_collection.hide_viewport = False


def goto_index(context, index):
    scene = context.scene
    if not scene.vr_landmarks:
        return False
    index = max(0, min(index, len(scene.vr_landmarks) - 1))
    landmark = scene.vr_landmarks[index]
    camera = landmark.base_pose_object
    if camera is not None:
        collections = [
            collection.name for collection in camera.users_collection]
        if collections:
            isolate_collection(collections[0])
            # Excluded collections can retain a stale matrix_world after a
            # file load. Refresh now so VR Preview and the readout immediately
            # see the newly selected camera at its authored location.
            context.view_layer.update()
        for obj in context.view_layer.objects:
            obj.select_set(False)
        if camera.name in context.view_layer.objects:
            camera.select_set(True)
            context.view_layer.objects.active = camera
            context.scene.camera = camera
    scene.vr_landmarks_selected = index
    scene.vr_landmarks_active = index
    return True


class REACH_OT_goto_seat(bpy.types.Operator):
    bl_idname = "reach.goto_vehicle_seat"
    bl_label = "Go to Reach seat"
    bl_description = "Show only this identity and put VR at its base camera"
    bl_options = {'REGISTER', 'UNDO'}

    index: bpy.props.IntProperty(default=0)

    def execute(self, context):
        if not goto_index(context, self.index):
            self.report({'WARNING'}, "No Reach seat landmarks")
            return {'CANCELLED'}
        return {'FINISHED'}


class REACH_OT_step_seat(bpy.types.Operator):
    bl_idname = "reach.step_vehicle_seat"
    bl_label = "Next/previous Reach seat"
    bl_options = {'REGISTER', 'UNDO'}

    delta: bpy.props.IntProperty(default=1)

    def execute(self, context):
        scene = context.scene
        if not scene.vr_landmarks:
            return {'CANCELLED'}
        count = len(scene.vr_landmarks)
        goto_index(context, (scene.vr_landmarks_active + self.delta) % count)
        return {'FINISHED'}


class REACH_OT_rebuild_seats(bpy.types.Operator):
    bl_idname = "reach.rebuild_vehicle_seats"
    bl_label = "Rebuild Reach seat list"
    bl_description = "Re-read cam: cameras without changing their positions"

    def execute(self, context):
        count = build_landmarks()
        apply_vr_defaults()
        self.report({'INFO'}, "%d Reach seats" % count)
        return {'FINISHED'}


class REACH_OT_mark_placed(bpy.types.Operator):
    bl_idname = "reach.mark_vehicle_seat_placed"
    bl_label = "Mark this seat placed"
    bl_description = (
        "Mark this camera ready for export; the seed reference is retained")
    bl_options = {'REGISTER', 'UNDO'}

    camera_name: bpy.props.StringProperty(default="")

    def execute(self, context):
        camera = bpy.data.objects.get(self.camera_name)
        if camera is None or camera.type != 'CAMERA':
            return {'CANCELLED'}
        camera["reach_needs_user_placement"] = False
        return {'FINISHED'}


def draw_camera_translation(layout, camera):
    """Draw the only transform components the runtime/exporter consumes."""
    box = layout.box()
    box.label(text="Eye position (metres)")
    box.use_property_split = True
    box.use_property_decorate = False
    box.prop(camera, "location", index=0, text="Forward (+X)")
    box.prop(camera, "location", index=1, text="Left (+Y)")
    box.prop(camera, "location", index=2, text="Up (+Z)")
    box.label(text="Rotation and scale stay locked")


class REACH_PT_vehicle_seats(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Reach Seats"
    bl_label = "Vehicle cameras"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        settings = getattr(
            context.window_manager, "xr_session_settings", None)

        row = layout.row(align=True)
        row.operator(
            "reach.step_vehicle_seat", text="Prev",
            icon='TRIA_LEFT').delta = -1
        row.operator(
            "reach.step_vehicle_seat", text="Next",
            icon='TRIA_RIGHT').delta = 1
        if settings is not None:
            layout.prop(
                settings, "use_positional_tracking",
                text="Let me lean (off = exact base)")
        layout.operator("reach.rebuild_vehicle_seats", icon='FILE_REFRESH')

        if not scene.vr_landmarks:
            layout.label(text="Press Rebuild", icon='ERROR')
            return

        active = min(
            scene.vr_landmarks_active, len(scene.vr_landmarks) - 1)
        camera = scene.vr_landmarks[active].base_pose_object
        if camera is not None:
            # Cameras are deliberately unparented, so location is the authored
            # world position even while another collection is excluded.
            position = camera.location
            vehicle, seat = camera_parts(camera)
            draw_camera_translation(layout, camera)
            if camera.get("reach_needs_user_placement", True):
                warn = layout.box()
                warn.alert = True
                warn.label(
                    text="Marker seed only - place the eye", icon='ERROR')
                op = warn.operator(
                    "reach.mark_vehicle_seat_placed", icon='CHECKMARK')
                op.camera_name = camera.name
            box = layout.box()
            box.label(text="Reach world units")
            box.label(text="x %+.4f  y %+.4f  z %+.4f" % (
                position.x / WU_TO_M,
                position.y / WU_TO_M,
                position.z / WU_TO_M))
            box.label(text="Config suffix: %s_%s" % (vehicle, seat))
            source = camera.get("reach_seed_source", "unknown")
            box.label(text="Initial HREK seed: %s" % source)

        layout.separator()
        column = layout.column(align=True)
        for index, landmark in enumerate(scene.vr_landmarks):
            row = column.row(align=True)
            row.alert = index == active
            op = row.operator(
                "reach.goto_vehicle_seat", text=landmark.name,
                icon='CAMERA_DATA' if index == active else 'DOT')
            op.index = index


CLASSES = (
    REACH_OT_goto_seat,
    REACH_OT_step_seat,
    REACH_OT_rebuild_seats,
    REACH_OT_mark_placed,
    REACH_PT_vehicle_seats,
)
_keymaps = []


def unregister():
    for keymap, item in _keymaps:
        try:
            keymap.keymap_items.remove(item)
        except Exception:
            pass
    _keymaps.clear()
    for cls in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    config = bpy.context.window_manager.keyconfigs.addon
    if config:
        keymap = config.keymaps.new(name='3D View', space_type='VIEW_3D')
        for key, delta in (('RIGHT_ARROW', 1), ('LEFT_ARROW', -1)):
            item = keymap.keymap_items.new(
                "reach.step_vehicle_seat", key, 'PRESS',
                ctrl=True, shift=True)
            item.properties.delta = delta
            _keymaps.append((keymap, item))


if __name__ == "__main__":
    unregister()
    if not ensure_addon():
        print(
            "VR Scene Inspection unavailable. Enable it, then run again.")
    else:
        register()
        count = build_landmarks()
        apply_vr_defaults()
        print(
            "Reach Seats ready: %d marker-seeded cameras. Press N in the "
            "3D View and choose Reach Seats." % count)
